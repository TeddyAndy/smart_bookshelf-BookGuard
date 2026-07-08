# 数据库模块 Makefile
# sqlite3 amalgamation 直接编译进程序，无需外部 lib

CC      = arm-none-linux-gnueabihf-gcc
CFLAGS  = -Wall -O2 -pthread -ldl -lm
TARGET  = db_test
OBJS    = db.o db_test.o sqlite3.o

# SQLite3 编译优化 (嵌入式场景)
SQLITE_FLAGS = \
    -DSQLITE_THREADSAFE=1 \
    -DSQLITE_ENABLE_FTS5 \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_USE_ALLOCA

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

db.o: db.c db.h sqlite3.h
	$(CC) $(CFLAGS) -c db.c -o $@

db_test.o: db_test.c db.h
	$(CC) $(CFLAGS) -c db_test.c -o $@

# sqlite3.c 很大 (~9MB)，单独编译，不加 -Wall 避免海量警告
sqlite3.o: sqlite3.c sqlite3.h
	$(CC) -O2 $(SQLITE_FLAGS) -c sqlite3.c -o $@

clean:
	rm -f $(TARGET) *.o

upload: $(TARGET)
	scp $(TARGET) root@192.168.0.232:/root/test/

.PHONY: all clean upload
