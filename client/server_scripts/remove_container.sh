sudo docker rm -fv $CONTAINER_NAME >/dev/null 2>&1;\
sudo docker rmi $CONTAINER_NAME >/dev/null 2>&1;\
sudo rm -rf $DOCKERFILE_FOLDER;\
! sudo docker inspect $CONTAINER_NAME >/dev/null 2>&1
