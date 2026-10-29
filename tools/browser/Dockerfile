FROM python:3.15-slim@sha256:94d3426f1def7db4a106a232ac121bfd975393e5dfd40a619b955fae45fe7913

RUN apt-get update \
    && apt-get install --no-install-recommends -y chromium fonts-liberation \
    && rm -rf /var/lib/apt/lists/*

ENV BROWSER_BIN=/usr/bin/chromium PYTHONDONTWRITEBYTECODE=1
WORKDIR /project
