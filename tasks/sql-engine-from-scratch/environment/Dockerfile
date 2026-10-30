FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    make \
    libc6-dev \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install pytest==7.4.4

# Create workspace and agent user
RUN useradd -m -s /bin/bash agent && \
    mkdir -p /workspace /logs/verifier && \
    chown -R agent:agent /workspace /logs

WORKDIR /workspace

# Copy workspace starter files
COPY workspace/ /workspace/

# Copy tests to root-only location
COPY tests/ /root/tests/

RUN chown -R agent:agent /workspace && \
    chmod -R 755 /workspace

USER agent
