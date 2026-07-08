#!/bin/bash
set -e
echo 9091900 | sudo -S docker run --rm alpine:3.20 sh -c "apk update >/dev/null 2>&1; apk info -e 3proxy && echo INSTALLED || echo NOT_INSTALLED; echo ---; apk search 3proxy"
