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
#define DUMP

static int new_state;
static sem_t wait_for_state;
static sem_t wait_for_parameters;
static int image_width=0;
static int image_height=0;

static OMX_BUFFERHEADERTYPE *omx_buffers_out[100];
static OMX_BUFFERHEADERTYPE *omx_buffers_in[100];

static OMX_HANDLETYPE decoderhandle;

static int buffer_out_pos;
static int buffer_out_requested;
static sem_t buffer_out_filled;
static int buffer_out_nb;
static int buffer_out_avp, buffer_out_ap;
static int buffer_out_size;

static int buffer_in_pos;
static int buffer_in_requested;
static sem_t buffer_in_filled;
static int buffer_in_nb;
static int buffer_in_avp, buffer_in_ap;
static int buffer_in_size;

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
	printf("empty\n");

	buffer_in_ap++;
	buffer_in_avp--;

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
	bufstate(out);
	buffer_out_ap++;
	buffer_out_avp--;

#ifdef DUMP
	write(dumper, pBuffer->pBuffer,  pBuffer->nFilledLen);
	fsync(dumper);
	printf("sync...\n");
#endif

#if 0
	int i;
	printf("dump: ->");
	for(i=0;i<pBuffer->nFilledLen;i++)
		printf("%02x ", *(pBuffer->pBuffer+i));

	printf("<-\n");
#endif

	sem_post(&buffer_out_filled);
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
	sem_init(&buffer_out_filled, 0, 0);

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

	int i;
	for(i=0;i<paramPort.nBufferCountMin;++i) {
		err = OMX_AllocateBuffer(decoderhandle, &omx_buffers_out[i], 1, NULL, paramPort.nBufferSize);
		OMXE(err);

#ifdef DEBUG
		printf("buf_out[%d]=%p\n", i, omx_buffers_out[i]);
#endif
	}
	

	buffer_out_nb = paramPort.nBufferCountMin;
	buffer_out_ap = buffer_out_nb;
	buffer_out_avp = 0;

	buffer_out_pos=0;
	buffer_out_size=paramPort.nBufferSize;

	// input buffers
	setHeader(&paramPort, sizeof(paramPort));
	paramPort.nPortIndex = 0;

	err=OMX_GetParameter(decoderhandle, OMX_IndexParamPortDefinition, &paramPort);
	OMXE(err);

#ifdef DEBUG
	printf("Requesting %d buffers of %d bytes\n", paramPort.nBufferCountMin, paramPort.nBufferSize);
#endif

	for(i=0;i<paramPort.nBufferCountMin;++i) {
		err = OMX_AllocateBuffer(decoderhandle, &omx_buffers_in[i], 0, NULL, paramPort.nBufferSize);
		OMXE(err);

#ifdef DEBUG
		printf("buf_in[%d]=%p\n", i, omx_buffers_in[i]);
#endif
	}



	buffer_in_nb = paramPort.nBufferCountMin;
	buffer_in_ap = buffer_in_nb;
	buffer_in_avp = 0;

	buffer_in_pos=0;
	buffer_in_size=paramPort.nBufferSize;


	
	printf("idle\n");
	GoToState(OMX_StateIdle);

	printf("go executing\n");
	GoToState(OMX_StateExecuting);

	
	return 1;
}

void decode() //void * data, int len)
{
	OMX_ERRORTYPE err;
	int size;
	OMX_BUFFERHEADERTYPE *buf;


	while(buffer_out_ap > 0) {
		bufstate(out);

		err = OMX_FillThisBuffer(decoderhandle, omx_buffers_out[buffer_out_pos % buffer_out_nb]);
		OMXE(err);

		buffer_out_ap--;
		buffer_out_avp++;

		buffer_out_pos++;
	}

	int read_len;
	while(buffer_in_ap > 0) {
		buf = omx_buffers_in[buffer_in_pos % buffer_in_nb];
		read_len = read(input, buf->pBuffer, 4096);
		printf("read: %d\n", read_len);
		buf->nFilledLen = read_len;

		bufstate(in);

		err = OMX_EmptyThisBuffer(decoderhandle, buf);
		OMXE(err);

		buffer_in_ap--;
		buffer_in_avp++;
		buffer_in_pos++;

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
