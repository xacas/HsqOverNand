#!/bin/bash
sudo rmmod nandsim
# parts=512,512,512 で256MBチップを4分割し、/dev/mtd0..3(各64MB)を作る。
# 残り領域が自動的に4個目のパーティションになる。1デバイス=1CPU=2レーン並列。
# 実機では接続されているフラッシュの数(/dev/mtd0..3)をnandServerが自動検出する
sudo modprobe nandsim first_id_byte=0x98 second_id_byte=0xda third_id_byte=0x90 fourth_id_byte=0x15 parts=512,512,512
#make clean; make
#time sudo ./subleq
sudo ./nandServer
