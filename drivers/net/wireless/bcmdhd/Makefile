# bcmdhd
DHDCFLAGS = -Wall -Wstrict-prototypes -Dlinux -DBCMDRIVER                     \
	-DBCMDONGLEHOST -DUNRELEASEDCHIP -DBCMDMA32 -DWLBTAMP -DBCMFILEIMAGE  \
	-DDHDTHREAD -DDHD_GPL -DDHD_SCHED -DDHD_DEBUG -DSDTEST -DBDC -DTOE    \
	-DDHD_BCMEVENTS -DSHOW_EVENTS -DDONGLEOVERLAYS -DBCMDBG               \
	-DCUSTOMER_HW2 -DCUSTOM_OOB_GPIO_NUM=2 -DOOB_INTR_ONLY -DHW_OOB       \
	-DMMC_SDIO_ABORT -DBCMSDIO -DBCMLXSDMMC -DBCMPLATFORM_BUS -DWLP2P     \
	-DNEW_COMPAT_WIRELESS -DWIFI_ACT_FRAME -DARP_OFFLOAD_SUPPORT          \
	-DKEEP_ALIVE -DCSCAN -DGET_CUSTOM_MAC_ENABLE -DPKT_FILTER_SUPPORT     \
	-DEMBEDDED_PLATFORM -DENABLE_INSMOD_NO_FW_LOAD -DPNO_SUPPORT          \
	-DOEM_ANDROID -DLINUX -DWL_ENABLE_P2P_IF -DWL_CFG80211_STA_EVENT -DHT40_GO  \
	-DSUPPORT_PM2_ONLY \
	-DMEIZU_FAST_SCAN \
	-DESCAN_RESULT_PATCH \
	-DWL_SCHED_SCAN \
	-Idrivers/net/wireless/bcmdhd -Idrivers/net/wireless/bcmdhd/include

DHDCFLAGS += -DBCMWAPI_WPI -DBCMWAPI_WAI

# bus:txglom mode

 # DHDCFLAGS += -DCUSTOM_GLOM_SETTING=7

  DHDCFLAGS += -DCONFIG_DHD_USE_STATIC_BUF
  DHDCFLAGS += -DDHD_USE_STATIC_BUF -DENHANCED_STATIC_BUF
  DHDCFLAGS += -DDHD_USE_IDLECOUNT

  DHDCFLAGS += -DPROP_TXSTATUS -DPROP_TXSTATUS_VSDB

  DHDCFLAGS += -DDHD_FIRSTREAD=128 -DDHD_SDALIGN=32 -DMAX_HDR_READ=128

  DHDCFLAGS += -DCUSTOM_SDIO_F2_BLKSIZE=512

# rxglom use chain
 DHDCFLAGS += -DCUSTOM_RXCHAIN=1

# enable rx_thread
  DHDCFLAGS += -DRXFRAME_THREAD

 DHDCFLAGS += -DUSE_KTHREAD_API
 DHDCFLAGS += -DCUSTOM_DPC_CPUCORE=0 -DCUSTOM_RXF_CPUCORE=2 -DCUSTOM_DPC_PRIO_SETTING=0


# txglom enable, maxtxglom size, increase txbound

  DHDCFLAGS += -DBCMSDIOH_TXGLOM -DCUSTOM_TXGLOM=1 -DBCMSDIOH_TXGLOM_HIGHSPEED

  DHDCFLAGS += -DCUSTOM_TXGLOM_SIZE=32 -DCUSTOM_MAX_TXGLOM_SIZE=32 -DDHD_TXBOUND=32

#  DHDCFLAGS += -DUSE_DYNAMIC_F2_BLKSIZE -DDYNAMIC_F2_BLKSIZE_FOR_NONLEGACY=128
#  DHDCFLAGS += -DREPEAT_READFRAME -DDHD_SET_FW_HIGHSPEED


# enable frameburst

  DHDCFLAGS += -DUSE_WL_FRAMEBURST

# inprove tcp ack

#  DHDCFLAGS += -DDHDTCPACK_SUPPRESS

# decrease ampdu size from 64 to 32
  DHDCFLAGS += -DCUSTOM_AMPDU_BA_WSIZE=32

#set roam trigger and delta
  DHDCFLAGS += -DROAM_ENABLE -DROAM_CHANNEL_CACHE
  DHDCFLAGS += -DCUSTOM_ROAM_TRIGGER_SETTING_B=-79 -DCUSTOM_ROAM_TRIGGER_SETTING_A=-76 -DCUSTOM_ROAM_DELTA_SETTING=15

DHDOFILES = aiutils.o bcmsdh_sdmmc_linux.o dhd_linux.o siutils.o bcmutils.o   \
	dhd_linux_sched.o dhd_sdio.o bcmwifi_channels.o bcmevent.o dhd_bta.o hndpmu.o  \
	bcmsdh.o dhd_cdc.o bcmsdh_linux.o dhd_common.o dhd_wlfc.o dhd_ip.o linux_osl.o \
	bcmsdh_sdmmc.o dhd_custom_gpio.o sbutils.o wldev_common.o wl_android.o dhd_pno.o dhd_cfg80211.o wl_roam.o

obj-$(CONFIG_BCMDHD) += bcmdhd.o
bcmdhd-objs += $(DHDOFILES)
ifneq ($(CONFIG_WIRELESS_EXT),)
bcmdhd-objs += wl_iw.o
DHDCFLAGS += -DSOFTAP
endif
ifneq ($(CONFIG_CFG80211),)
bcmdhd-objs += wl_cfg80211.o wl_cfgp2p.o wl_linux_mon.o
DHDCFLAGS += -DWL_CFG80211
endif
EXTRA_CFLAGS = $(DHDCFLAGS)
ifeq ($(CONFIG_BCMDHD),m)
EXTRA_LDFLAGS += --strip-debug
endif
