FROM python:3.13-slim@sha256:57b29a7c050c4e679e4bea572e14b3965cf4b5839de2da49d55e49ffc4024f39

RUN apt-get update \
    && apt-get install --no-install-recommends -y chromium fonts-liberation \
    && rm -rf /var/lib/apt/lists/*

ENV BROWSER_BIN=/usr/bin/chromium PYTHONDONTWRITEBYTECODE=1
WORKDIR /project
