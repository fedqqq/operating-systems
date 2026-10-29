FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ python3 rsyslog procps ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /lab
CMD ["sh", "run_tests.sh", "--isolated-syslog"]
