rainback: server.c
	$(CXX) -lssl -lcrypto $< -o$@

certificate.pem key.pem:
	openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 365 -out certificate.pem

run: rainback certificate.pem key.pem
	./$<
.PHONY: run

clean:
	rm rainback
.PHONY: clean
