PREFIX=/usr/local
CC = gcc
CFLAGS = -pedantic -Wall -g -pthread -ludev
LDFLAGS = -pthread -ludev
CFLAGS_SO = -pedantic -Wall -fPIC -g -pthread
LDFLAGS_SO = -ldl -pthread

lib = faps-lib
l32 = faps-lib32
dae = faps-daemon

.PHONY: main
main: $(dae) $(lib).so

$(dae): src/$(dae).o
	$(CC) $(LDFLAGS) -o $@ $<

src/$(dae).o: src/$(dae).c
	$(CC) $(CFLAGS) -o $@ -c $<

$(lib).so: src/$(lib).o
	$(CC) $(LDFLAGS_SO) -o $@ -shared $<

src/$(lib).o: src/$(lib).c
	$(CC) $(CFLAGS_SO) -o $@ -c $<

$(l32).so: src/$(l32).o
	$(CC) -m32 $(LDFLAGS_SO) -o $@ -shared $<

src/$(l32).o: src/$(lib).c
	$(CC) -m32 $(CFLAGS_SO) -o $@ -c $<

.PHONY: 32
32: $(dae) $(l32).so

.PHONY: all
all: main 32

.PHONY: clean
clean:
	rm -f $(fs) $(dae).so /src/$(dae).o /src/$(lib).o
	-rm -f $(l32).so /src/$(l32).o
	
.PHONY: install
install:
	cp $(lib).so $(PREFIX)/lib/$(lib).so
	-cp $(l32).so $(PREFIX)/lib/$(l32).so
	cp $(dae) $(PREFIX)/bin/$(dae)
	cp faps $(PREFIX)/bin/faps
	sudo setcap "CAP_DAC_READ_SEARCH+pe" $(PREFIX)/bin/$(dae)

.PHONY: uninstall
uninstall:
	rm $(PREFIX)/lib/$(lib).so
	-rm $(PREFIX)/lib/$(l32).so
	rm $(PREFIX)/bin/$(dae)
	rm $(PREFIX)/bin/faps
