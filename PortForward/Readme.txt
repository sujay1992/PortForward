PortForward.exe              # uses settings.txt in current directory
PortForward.exe myconf.txt   # custom settings file


g++ -std=c++11 -O2 -pthread -o port_forward port_forward_linux.cpp

./port_forward               # uses settings.txt in current directory
./port_forward myconf.txt    # custom settings file