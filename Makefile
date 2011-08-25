
all: nv

nv%: demo%.c
	gcc $< -o $@ -I/usr/include/OpenMAX/IL -lrt -lnvomx -lnvrm_graphics_impl -g
	

run: nv test.mp4
	./nv
