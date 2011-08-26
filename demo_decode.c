#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>

#include <sys/stat.h>
#include <fcntl.h>


#include <OpenMAX/IL/OMX_Core.h>
#include <OpenMAX/IL/OMX_Component.h>
#include <OpenMAX/IL/OMX_Types.h>

#define COMPONENT "OMX.Nvidia.h264.decode"

#define DEBUG
//#define DUMP

static int new_state;
static sem_t wait_for_state;
static sem_t wait_for_parameters;
static sem_t wait_buff;
static int image_width=0;
static int image_height=0;

static OMX_BUFFERHEADERTYPE **omx_buffers_out;
static OMX_BUFFERHEADERTYPE **omx_buffers_in;

static OMX_HANDLETYPE decoderhandle;

static int buffer_out_mask, buffer_out_nb;
static int buffer_in_mask, buffer_in_nb;

int dumper, input; // write out fd

#define OMXE(e) if(e != OMX_ErrorNone) \
			fprintf(stderr, "EE:%s:%d:%x\n", __FUNCTION__, __LINE__, e);


#define bufstate(_d) \
printf(#_d": %d %d %d %x\n", \
		buffer_##_d##_ap, \
		buffer_##_d##_avp, \
		buffer_##_d##_pos%buffer_##_d##_nb, \
		\
		omx_buffers_##_d[ \
			buffer_##_d##_pos % buffer_##_d##_nb \
		] \
	)

static void setHeader(OMX_PTR header, OMX_U32 size) {
	OMX_VERSIONTYPE* ver = (OMX_VERSIONTYPE*)(header + sizeof(OMX_U32));
	*((OMX_U32*)header) = size;

	ver->s.nVersionMajor = 1;
	ver->s.nVersionMinor = 1;
	ver->s.nRevision = 0;
	ver->s.nStep = 0;
}

static OMX_ERRORTYPE decoderEmptyBufferDone(
		OMX_OUT OMX_HANDLETYPE hComponent,
		OMX_OUT OMX_PTR pAppData,
		OMX_OUT OMX_BUFFERHEADERTYPE* pBuffer) {

	printf("empty %x\n", pBuffer->pPlatformPrivate);
	if(pBuffer->pPlatformPrivate < 102400)
		exit(1);

	buffer_in_mask |= 1<<*(short*)pBuffer->pPlatformPrivate;
	sem_post(&wait_buff);

	return 0;
}

static OMX_ERRORTYPE decoderFillBufferDone(
		OMX_OUT OMX_HANDLETYPE hComponent,
		OMX_OUT OMX_PTR pAppData,
		OMX_OUT OMX_BUFFERHEADERTYPE* pBuffer) {

	static int got = 0, times=0;

	times++;

	got +=  pBuffer->nFilledLen,
	printf("filled %x %d %d %d\n", pBuffer,  pBuffer->nFilledLen, got, times );

#ifdef DUMP
	write(dumper, pBuffer->pBuffer,  pBuffer->nFilledLen);
	fsync(dumper);
	printf("sync...\n");
#endif

	buffer_out_mask |= 1<<*(short*)pBuffer->pPlatformPrivate;

	sem_post(&wait_buff);

#if 0
	int i;
	printf("dump: ->");
	for(i=0;i<pBuffer->nFilledLen;i++)
		printf("%02x ", *(pBuffer->pBuffer+i));

	printf("<-\n");
#endif

	return 0;
}

static OMX_ERRORTYPE decoderEventHandler(
		OMX_OUT OMX_HANDLETYPE hComponent,
		OMX_OUT OMX_PTR pAppData,
		OMX_OUT OMX_EVENTTYPE eEvent,
		OMX_OUT OMX_U32 Data1,
		OMX_OUT OMX_U32 Data2,
		OMX_OUT OMX_PTR pEventData) {
	printf("VD_OMX:Got event %x %x %x\n", eEvent, (unsigned int)Data1, (unsigned int)Data2);

	switch(eEvent) {
	case OMX_EventCmdComplete:
		switch((OMX_COMMANDTYPE)Data1) {
		case OMX_CommandStateSet:

			printf("state switch %d\n", new_state);
			new_state = Data2;
			sem_post(&wait_for_state);
			return 0;
		}
	case OMX_EventPortSettingsChanged:
		/*
		if(Data1 != 1) {
			fprintf(stderr, "got a INPUT port changed event !\n");
			return 0;
		}

		OMX_ERRORTYPE err;
		OMX_PARAM_PORTDEFINITIONTYPE paramPort;
		setHeader(&paramPort, sizeof(paramPort));
		paramPort.nPortIndex = 1;

		err=OMX_GetParameter(decoderhandle, OMX_IndexParamPortDefinition, &paramPort);

		if(err != OMX_ErrorNone) 
			fprintf(stderr, "EE:%s:%d\n", __FUNCTION__, __LINE__);


		image_width=paramPort.format.video.nFrameWidth;
		image_height=paramPort.format.video.nFrameHeight;

		buffer_out_size=paramPort.nBufferSize;
		sem_post(&wait_for_parameters);


		fprintf(stderr, "buffer size = %d\n", buffer_out_size);
		fprintf(stderr, "FPS=%d*2**16 + %d\n", paramPort.format.video.xFramerate>>16, paramPort.format.video.xFramerate&0xffff);
		*/
		printf("ou~\n");
		break;
	case OMX_EventBufferFlag:
		printf("thas alll\n");

#ifdef DUMP
		close(dumper);
#endif
		exit(0);
	case OMX_EventError:
		printf("fail\n");
		exit(1);
	}
	return 0;
}

static OMX_CALLBACKTYPE decodercallbacks = {
	.EventHandler = decoderEventHandler,
	.EmptyBufferDone = decoderEmptyBufferDone,
	.FillBufferDone = decoderFillBufferDone,
};

static inline void _GoToState(int state) {

	OMX_ERRORTYPE err;
	err = OMX_SendCommand(decoderhandle, OMX_CommandStateSet, state, NULL);
	OMXE(err);
	
};

static void GoToState(int state) {
	int sval;

	//Is there a more secure way ?
	new_state=state;
	do {
		sem_getvalue(&wait_for_state, &sval);
	} while(sval);

	_GoToState(state);
	sem_wait(&wait_for_state);
}

static int init()
{
	OMX_ERRORTYPE err;
	OMX_PARAM_PORTDEFINITIONTYPE paramPort;
	err = OMX_Init();
	printf("om init %d\n", err);
	if (err != OMX_ErrorNone) {
		return 0;
	}
	sem_init(&wait_for_state, 0, 0);
	sem_init(&wait_for_parameters, 0, 0);
	sem_init(&wait_buff, 0, 0);

	err = OMX_GetHandle(&decoderhandle, COMPONENT, NULL, &decodercallbacks);
	OMXE(err);


	// output buffers
	setHeader(&paramPort, sizeof(paramPort));
	paramPort.nPortIndex = 1;

	err=OMX_GetParameter(decoderhandle, OMX_IndexParamPortDefinition, &paramPort);
	OMXE(err);


/*
 * Codec=7
 * Color=19
 * Size=1920x816
 * */

#ifdef DEBUG
	printf("Requesting %d buffers of %d bytes\n", paramPort.nBufferCountMin, paramPort.nBufferSize);
#endif

	paramPort.nBufferSize=848*480*3/2;
	paramPort.format.video.nFrameWidth = 848;
	paramPort.format.video.nFrameHeight = 480;;

	err=OMX_SetParameter(decoderhandle, OMX_IndexParamPortDefinition, &paramPort);
	OMXE(err);

	omx_buffers_out = malloc(sizeof(OMX_BUFFERHEADERTYPE*) * paramPort.nBufferCountMin);

	int i;
	short *count = malloc(paramPort.nBufferCountMin+100);

	for(i=0;i<paramPort.nBufferCountMin;++i) {
		err = OMX_AllocateBuffer(decoderhandle, &omx_buffers_out[i], 1, NULL, paramPort.nBufferSize);
		OMXE(err);

		buffer_out_mask |= 1<<i;

		omx_buffers_out[i]->pPlatformPrivate = count;
		*count = i;
		count++;


#ifdef DEBUG
		printf("buf_out[%d]=%p\n", i, omx_buffers_out[i]);
#endif
	}
	

	buffer_out_nb = paramPort.nBufferCountMin;

	// input buffers
	setHeader(&paramPort, sizeof(paramPort));
	paramPort.nPortIndex = 0;

	err=OMX_GetParameter(decoderhandle, OMX_IndexParamPortDefinition, &paramPort);
	OMXE(err);

#ifdef DEBUG
	printf("Requesting %d buffers of %d bytes\n", paramPort.nBufferCountMin, paramPort.nBufferSize);
#endif

	omx_buffers_in = malloc(sizeof(OMX_BUFFERHEADERTYPE*) * paramPort.nBufferCountMin);

	count = malloc(paramPort.nBufferCountMin+100);


	for(i=0;i<paramPort.nBufferCountMin;++i) {
		err = OMX_AllocateBuffer(decoderhandle, &omx_buffers_in[i], 0, NULL, paramPort.nBufferSize);
		OMXE(err);

		buffer_in_mask |= 1<<i;

		omx_buffers_in[i]->pPlatformPrivate = count;
		*count = i;
		count++;

#ifdef DEBUG
		printf("buf_in[%d]=%p\n", i, omx_buffers_in[i]);
#endif
	}



	buffer_in_nb = paramPort.nBufferCountMin;

	
	printf("idle\n");
	GoToState(OMX_StateIdle);

	printf("go executing\n");
	GoToState(OMX_StateExecuting);

	
	return 1;
}

void decode() //void * data, int len)
{
	OMX_ERRORTYPE err;
	int size, i;
	OMX_BUFFERHEADERTYPE *buf;

	if(!(buffer_in_mask || buffer_out_mask)) {
		printf("wait damn avp\n");
		sem_wait(&wait_buff);
	}

	for(i=0;i<buffer_out_nb;i++) {

		printf("<out mask: %x i=%d\n",  buffer_out_mask, i );

		if( ! ((1<<i) & buffer_out_mask ) )
			continue;

		err = OMX_FillThisBuffer(decoderhandle, omx_buffers_out[i]);
		OMXE(err);

		buffer_out_mask &= (1<<i) ^ 0xFFFFFFFF;
		
		printf(">out mask: %x i=%d\n",  buffer_out_mask, i );

	}

	int read_len;
	
	for(i=0;i<buffer_in_nb;i++) {


		buf = omx_buffers_in[i];

		printf("<in mask: %x i=%d\n",  buffer_in_mask, i );

		if( ! ((1<<i) & buffer_in_mask ) )
			continue;

		read_len = read(input, buf->pBuffer, 4096);
		printf("read: %d\n", read_len);
		buf->nFilledLen = read_len;

		err = OMX_EmptyThisBuffer(decoderhandle, buf);
		OMXE(err);


		buffer_in_mask &= (1<<i) ^ 0xFFFFFFFF;
		
		printf(">in mask: %x i=%d\n",  buffer_in_mask, i );

	}


	/*
	printf("wait filled cb\n");
	sem_wait(&buffer_out_filled);
	printf("ok...\n");
	*/


}

int main() {
	input = open("test.h264", O_RDONLY);
#ifdef DUMP
	dumper=open("dump.out", O_WRONLY | O_CREAT);
#endif

	init();

	while(1)
		decode();
}
