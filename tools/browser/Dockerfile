FROM python:3.15-slim@sha256:e97b88e416c3c01eb427b21fd8cd3ebf006e6ee38afcfcc3cae1c54ee2ab0a75

RUN apt-get update \
    && apt-get install --no-install-recommends -y chromium fonts-liberation \
    && rm -rf /var/lib/apt/lists/*

ENV BROWSER_BIN=/usr/bin/chromium PYTHONDONTWRITEBYTECODE=1
WORKDIR /project
