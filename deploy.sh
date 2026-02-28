tmux new -s static-server
/home/ubuntu/Tudou/build/examples/StaticFileHttpServer/static-server -r /home/ubuntu/Tudou/configs/static-file-http-server

tmux new -s filelink-server
/home/ubuntu/Tudou/build/examples/FileLinkServer/filelink-server -r /home/ubuntu/Tudou/configs/file-link-server

tmux new -s starmind
/home/ubuntu/Tudou/build/examples/StarMind/StarMind -r /home/ubuntu/Tudou/configs/starmind

tmux new -s docker
cd /home/wxm/Tudou && sudo docker compose up -d