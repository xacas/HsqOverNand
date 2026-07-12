#!/bin/bash
sudo rmmod nandsim
sudo modprobe nandsim first_id_byte=0x98 second_id_byte=0xda third_id_byte=0x90 fourth_id_byte=0x15
#make clean; make
#time sudo ./subleq
sudo ./nandServer
