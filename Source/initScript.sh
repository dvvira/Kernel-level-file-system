#!/bin/bash

#This script will setup the basic environment for the dedupe file system

#	* This code is submitted by 
#	* Denil Vira (dvvira), 
#	* Nitin Tak (ntak), 
#	* Pooja Routray (proutra), 
#	* Sumeet Hemant Bhatia (sbhatia3) 
#	* as a part of CSC 568 - 001 Spring 2015

mkdir FSDevice
mkdir UserSpace
mkdir -p $1

make clean
make

dd bs=4096 count=4096 if=/dev/zero of=FSDevice/device

gcc -o UserSpace/makekfs_dedupefs makekfs_dedupefs.c
./UserSpace/makekfs_dedupefs FSDevice/device

insmod dedupe_fs.ko

mount -o loop,owner,group,users -t dedupefs FSDevice/device $1
