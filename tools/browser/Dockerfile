FROM python:3.15-slim@sha256:08a4c22c92b79ec821a7ad49768480039ee5b7accac3955228e75366e30abc8e

RUN apt-get update \
    && apt-get install --no-install-recommends -y chromium fonts-liberation \
    && rm -rf /var/lib/apt/lists/*

ENV BROWSER_BIN=/usr/bin/chromium PYTHONDONTWRITEBYTECODE=1
WORKDIR /project
