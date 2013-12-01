CC=gcc
CFLAGS=-I.
DEPS = pl330_vfio_driver/pl330_vfio.h
OBJ = pl330_vfio_driver/pl330_vfio.o test_pl330_vfio_driver.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

test_pl330_vfio_driver: $(OBJ)
	gcc -o $@ $^ $(CFLAGS)

clean:
	rm -f pl330_vfio_driver/pl330_vfio.o test_pl330_vfio_driver test_pl330_vfio_driver.o
