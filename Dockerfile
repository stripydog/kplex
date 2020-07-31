FROM debian:10-slim

# Install build tools
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential git \
    && rm -rf /var/lib/apt/lists/*

# Do not verify certificates
RUN git config --global http.sslverify false

# Install kplex
WORKDIR /app
COPY . .
RUN make /app
RUN make install /app

# Remove source code
RUN rm -r /app

# Entrypoint
CMD /usr/bin/kplex
