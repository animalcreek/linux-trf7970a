#
# Makefile for HSR
#

obj-$(CONFIG_HSR_PRP)	+= hsr-prp.o

hsr-prp-y		:= hsr_main.o hsr_framereg.o hsr_device.o \
			   hsr_netlink.o hsr_slave.o hsr_forward.o
hsr-prp-$(CONFIG_DEBUG_FS) += hsr_prp_debugfs.o
