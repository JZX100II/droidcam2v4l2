CC=gcc
CFLAGS=
LIBS=-ldroidmedia -lyuv
SRC=src/droidcam2v4l2.c
TARGET=droidcam2v4l2

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)
