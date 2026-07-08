echo 9091900 | sudo -S sh -c 'docker run --rm alpine:3.20 sh -c "apk update >/dev/null 2>&1; apk info -e 3proxy; apk search ^3proxy"' 2>&1 | tail -30
