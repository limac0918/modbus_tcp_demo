FROM ubuntu:24.04

COPY gateway /usr/local/bin/gateway
COPY docker_libs/ /usr/local/lib/

RUN chmod +x /usr/local/bin/gateway && \
    ldconfig

EXPOSE 4840
ENTRYPOINT ["/usr/local/bin/gateway"]
