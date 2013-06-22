all: srcon
	

srcon: srcon.c
	$(CC) -lreadline srcon.c -o srcon
	strip srcon

debug: srcon.c
	$(CC) -g -Wall -lreadline srcon.c -o srcon

clean:
	rm srcon
