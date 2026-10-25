find_package(OpenSSL  REQUIRED)   # RSA-PSS signing, TLS for the hand-rolled WebSocket
find_package(CURL     REQUIRED)   # HTTPS REST (keep-alive; CURLINFO_NUM_CONNECTS gives the
                                  # "was this connection reused" flag the retry rule needs)
find_package(ZLIB     REQUIRED)
find_package(Threads  REQUIRED)
