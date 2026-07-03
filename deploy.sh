tmux new -s static-server
/home/ubuntu/Tudou/build/examples/StaticFileHttpServer/static-server -r /home/ubuntu/Tudou/configs/static-file-http-server

tmux new -s starmind
export STARMIND_API_KEY=...
/home/ubuntu/Tudou/build/examples/StarMind/StarMind -r /home/ubuntu/Tudou/configs/starmind
