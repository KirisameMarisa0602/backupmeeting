TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS += client server

client.file = client/client.pro
server.file = server/server.pro

# 如果存在先后依赖（一般不需要），可启用：
# server.depends =
# client.depends = server
