rainback: epoll.c client.c
	$(CXX) -lssl -lcrypto $^ -o$@

certificate.pem key.pem:
	openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 365 -out certificate.pem

PORT=4433

run: rainback certificate.pem key.pem
	./$< $(PORT)
.PHONY: run

check: certificate.pem
	./run-tests.sh $(PORT)
.PHONY: check

clean:
	rm rainback
.PHONY: clean
