INCLUDES = \
		   -I../common_src \
		   -I../proto_src

AM_CXXFLAGS =

game_LDADD = -lpthread -levent_pthreads -lprotobuf

bin_PROGRAMS = game 

game_SOURCES = \
			   ../common_src/net_thread.cpp \
			   ../common_src/msg_protobuf.cpp \
			   ../common_src/connection.cpp \
			   ../common_src/listener.cpp \
			   ../common_src/connector.cpp \
			   ../common_src/accept_cb.cpp \
			   ../common_src/read_cb.cpp \
			   ../common_src/write_cb.cpp \
			   ../common_src/conn_event_cb.cpp \
			   ../common_src/connector_event_cb.cpp \
			   ../common_src/log.cpp \
			   ../proto_src/cmd_util.cpp \
			   ../proto_src/login.pb.cc \
			   gate_cb.cpp \
			   center_cb.cpp \
			   main.cpp

