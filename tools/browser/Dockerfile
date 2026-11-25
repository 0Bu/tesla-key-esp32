FROM python:3.15-slim@sha256:63bb298d2b7b88f176f51b5189f54ce6535c997682ea70885d2132f5d473a8a5

RUN apt-get update \
    && apt-get install --no-install-recommends -y chromium fonts-liberation \
    && rm -rf /var/lib/apt/lists/*

ENV BROWSER_BIN=/usr/bin/chromium PYTHONDONTWRITEBYTECODE=1
WORKDIR /project
