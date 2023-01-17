//-----------------------------------------------------------------------------
// 2022 Ahoy, https://ahoydtu.de
// Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//-----------------------------------------------------------------------------

#ifndef __PAYLOAD_H__
#define __PAYLOAD_H__

#include "../utils/dbg.h"
#include "../utils/crc.h"
#include "../utils/handler.h"
#include "../config/config.h"
#include <Arduino.h>

typedef struct {
    uint8_t txCmd;
    uint8_t txId;
    uint8_t invId;
    uint32_t ts;
    uint8_t data[MAX_PAYLOAD_ENTRIES][MAX_RF_PAYLOAD_SIZE];
    uint8_t len[MAX_PAYLOAD_ENTRIES];
    bool complete;
    uint8_t maxPackId;
    bool lastFound;
    uint8_t retransmits;
    bool requested;
    bool gotFragment;
} invPayload_t;


typedef std::function<void(uint8_t)> payloadListenerType;


template<class HMSYSTEM>
class Payload : public Handler<payloadListenerType> {
    public:
        Payload() : Handler() {}

        void setup(IApp *app, HMSYSTEM *sys, statistics_t *stat, uint8_t maxRetransmits, uint32_t *timestamp) {
            mApp        = app;
            mSys        = sys;
            mStat       = stat;
            mMaxRetrans = maxRetransmits;
            mTimestamp  = timestamp;
            for(uint8_t i = 0; i < MAX_NUM_INVERTERS; i++) {
                reset(i);
            }
            mSerialDebug  = false;
            mHighPrioIv = NULL;
        }

        void enableSerialDebug(bool enable) {
            mSerialDebug = enable;
        }

        void notify(uint8_t val) {
            for(typename std::list<payloadListenerType>::iterator it = mList.begin(); it != mList.end(); ++it) {
               (*it)(val);
            }
        }

        void loop() {
            if(NULL != mHighPrioIv) {
                ivSend(mHighPrioIv, true);
                mHighPrioIv = NULL;
            }
        }

        void ivSendHighPrio(Inverter<> *iv) {
            mHighPrioIv = iv;
        }

        void ivSend(Inverter<> *iv, bool highPrio = false) {
            if(!highPrio) {
                if (mPayload[iv->id].requested) {
                    if (!mPayload[iv->id].complete)
                        process(false); // no retransmit

                    if (!mPayload[iv->id].complete) {
                        if (MAX_PAYLOAD_ENTRIES == mPayload[iv->id].maxPackId)
                            mStat->rxFailNoAnser++; // got nothing
                        else
                            mStat->rxFail++; // got fragments but not complete response

                        iv->setQueuedCmdFinished();  // command failed
                        if (mSerialDebug)
                            DPRINTLN(DBG_INFO, F("enqueued cmd failed/timeout"));
                        if (mSerialDebug) {
                            DPRINT(DBG_INFO, F("(#") + String(iv->id) + ") ");
                            DPRINTLN(DBG_INFO, F("no Payload received! (retransmits: ") + String(mPayload[iv->id].retransmits) + ")");
                        }
                    }
                }
            }

            reset(iv->id);
            mPayload[iv->id].requested = true;

            yield();
            if (mSerialDebug)
                DPRINTLN(DBG_INFO, F("(#") + String(iv->id) + F(") Requesting Inv SN ") + String(iv->config->serial.u64, HEX));

            if (iv->getDevControlRequest()) {
                if (mSerialDebug)
                    DPRINTLN(DBG_INFO, F("(#") + String(iv->id) + F(") Devcontrol request 0x") + String(iv->devControlCmd, HEX) + F(" power limit ") + String(iv->powerLimit[0]));
                mSys->Radio.sendControlPacket(iv->radioId.u64, iv->devControlCmd, iv->powerLimit);
                mPayload[iv->id].txCmd = iv->devControlCmd;
                //iv->clearCmdQueue();
                //iv->enqueCommand<InfoCommand>(SystemConfigPara); // read back power limit
            } else {
                uint8_t cmd = iv->getQueuedCmd();
                DPRINTLN(DBG_INFO, F("(#") + String(iv->id) + F(") sendTimePacket")); // + String(cmd, HEX));
                mSys->Radio.sendTimePacket(iv->radioId.u64, cmd, mPayload[iv->id].ts, iv->alarmMesIndex);
                mPayload[iv->id].txCmd = cmd;
            }
        }

        void add(packet_t *p, uint8_t len) {
            Inverter<> *iv = mSys->findInverter(&p->packet[1]);

            if(NULL == iv)
                return;

            if (p->packet[0] == (TX_REQ_INFO + ALL_FRAMES)) {  // response from get information command
                mPayload[iv->id].txId = p->packet[0];
                DPRINTLN(DBG_DEBUG, F("Response from info request received"));
                uint8_t *pid = &p->packet[9];
                if (*pid == 0x00) {
                    DPRINT(DBG_DEBUG, F("fragment number zero received and ignored"));
                } else {
                    DPRINTLN(DBG_DEBUG, "PID: 0x" + String(*pid, HEX));
                    if ((*pid & 0x7F) < MAX_PAYLOAD_ENTRIES) {
                        memcpy(mPayload[iv->id].data[(*pid & 0x7F) - 1], &p->packet[10], len - 11);
                        mPayload[iv->id].len[(*pid & 0x7F) - 1] = len - 11;
                        mPayload[iv->id].gotFragment = true;
                    }

                    if ((*pid & ALL_FRAMES) == ALL_FRAMES) {
                        // Last packet
                        if (((*pid & 0x7f) > mPayload[iv->id].maxPackId) || (MAX_PAYLOAD_ENTRIES == mPayload[iv->id].maxPackId)) {
                            mPayload[iv->id].maxPackId = (*pid & 0x7f);
                            if (*pid > 0x81)
                                mPayload[iv->id].lastFound = true;
                        }
                    }
                }
            } else if (p->packet[0] == (TX_REQ_DEVCONTROL + ALL_FRAMES)) { // response from dev control command
                DPRINTLN(DBG_DEBUG, F("Response from devcontrol request received"));

                mPayload[iv->id].txId = p->packet[0];
                iv->clearDevControlRequest();

                if ((p->packet[12] == ActivePowerContr) && (p->packet[13] == 0x00)) {
                    String msg = "";
                    if((p->packet[10] == 0x00) && (p->packet[11] == 0x00))
                        mApp->setMqttPowerLimitAck(iv);
                    else
                        msg = "NOT ";
                    DPRINTLN(DBG_INFO, F("Inverter ") + String(iv->id) + F(" has ") + msg + F("accepted power limit set point ") + String(iv->powerLimit[0]) + F(" with PowerLimitControl ") + String(iv->powerLimit[1]));
                    iv->clearCmdQueue();
                    iv->enqueCommand<InfoCommand>(SystemConfigPara); // read back power limit
                }
                iv->devControlCmd = Init;
            }
        }

        void process(bool retransmit) {
            for (uint8_t id = 0; id < mSys->getNumInverters(); id++) {
                Inverter<> *iv = mSys->getInverterByPos(id);
                if (NULL == iv)
                    continue; // skip to next inverter

                if ((mPayload[iv->id].txId != (TX_REQ_INFO + ALL_FRAMES)) && (0 != mPayload[iv->id].txId)) {
                    // no processing needed if txId is not 0x95
                    mPayload[iv->id].complete = true;
                    continue; // skip to next inverter
                }

                if (!mPayload[iv->id].complete) {
                    if (!build(iv->id)) { // payload not complete
                        if ((mPayload[iv->id].requested) && (retransmit)) {
                            if (iv->devControlCmd == Restart || iv->devControlCmd == CleanState_LockAndAlarm) {
                                // This is required to prevent retransmissions without answer.
                                DPRINTLN(DBG_INFO, F("Prevent retransmit on Restart / CleanState_LockAndAlarm..."));
                                mPayload[iv->id].retransmits = mMaxRetrans;
                            } else if(iv->devControlCmd == ActivePowerContr) {
                                DPRINTLN(DBG_INFO, F("retransmit power limit"));
                                mSys->Radio.sendControlPacket(iv->radioId.u64, iv->devControlCmd, iv->powerLimit);
                            } else {
                                if (mPayload[iv->id].retransmits < mMaxRetrans) {
                                    mPayload[iv->id].retransmits++;
                                    if(false == mPayload[iv->id].gotFragment) {
                                        DPRINTLN(DBG_WARN, F("nothing received: Request Complete Retransmit"));
                                        mPayload[iv->id].txCmd = iv->getQueuedCmd();
                                        DPRINTLN(DBG_INFO, F("(#") + String(iv->id) + F(") sendTimePacket 0x") + String(mPayload[iv->id].txCmd, HEX));
                                        mSys->Radio.sendTimePacket(iv->radioId.u64, mPayload[iv->id].txCmd, mPayload[iv->id].ts, iv->alarmMesIndex);
                                    } else {
                                        for (uint8_t i = 0; i < (mPayload[iv->id].maxPackId - 1); i++) {
                                            if (mPayload[iv->id].len[i] == 0) {
                                                DPRINTLN(DBG_WARN, F("Frame ") + String(i + 1) + F(" missing: Request Retransmit"));
                                                mSys->Radio.sendCmdPacket(iv->radioId.u64, TX_REQ_INFO, (SINGLE_FRAME + i), true);
                                                break;  // only request retransmit one frame per loop
                                            }
                                            yield();
                                        }
                                    }
                                    mSys->Radio.switchRxCh(100);
                                }
                            }
                        }
                    } else {  // payload complete
                        DPRINTLN(DBG_INFO, F("procPyld: cmd:  0x") + String(mPayload[iv->id].txCmd, HEX));
                        DPRINTLN(DBG_INFO, F("procPyld: txid: 0x") + String(mPayload[iv->id].txId, HEX));
                        DPRINTLN(DBG_DEBUG, F("procPyld: max:  ") + String(mPayload[iv->id].maxPackId));
                        record_t<> *rec = iv->getRecordStruct(mPayload[iv->id].txCmd);  // choose the parser
                        mPayload[iv->id].complete = true;

                        uint8_t payload[128];
                        uint8_t payloadLen = 0;

                        memset(payload, 0, 128);

                        for (uint8_t i = 0; i < (mPayload[iv->id].maxPackId); i++) {
                            memcpy(&payload[payloadLen], mPayload[iv->id].data[i], (mPayload[iv->id].len[i]));
                            payloadLen += (mPayload[iv->id].len[i]);
                            yield();
                        }
                        payloadLen -= 2;

                        if (mSerialDebug) {
                            DPRINT(DBG_INFO, F("Payload (") + String(payloadLen) + "): ");
                            mSys->Radio.dumpBuf(NULL, payload, payloadLen);
                        }

                        if (NULL == rec) {
                            DPRINTLN(DBG_ERROR, F("record is NULL!"));
                        } else if ((rec->pyldLen == payloadLen) || (0 == rec->pyldLen)) {
                            if (mPayload[iv->id].txId == (TX_REQ_INFO + 0x80))
                                mStat->rxSuccess++;

                            rec->ts = mPayload[iv->id].ts;
                            for (uint8_t i = 0; i < rec->length; i++) {
                                iv->addValue(i, payload, rec);
                                yield();
                            }
                            iv->doCalculations();
                            notify(mPayload[iv->id].txCmd);

                            if(AlarmData == mPayload[iv->id].txCmd) {
                                uint8_t i = 0;
                                while(1) {
                                    if(!iv->parseAlarmLog(i++, payload, payloadLen))
                                        break;
                                    yield();
                                }
                            }
                        } else {
                            DPRINTLN(DBG_ERROR, F("plausibility check failed, expected ") + String(rec->pyldLen) + F(" bytes"));
                            mStat->rxFail++;
                        }

                        iv->setQueuedCmdFinished();
                    }
                }

                yield();

            }
        }

    private:
        bool build(uint8_t id) {
            DPRINTLN(DBG_VERBOSE, F("build"));
            uint16_t crc = 0xffff, crcRcv = 0x0000;
            if (mPayload[id].maxPackId > MAX_PAYLOAD_ENTRIES)
                mPayload[id].maxPackId = MAX_PAYLOAD_ENTRIES;

            for (uint8_t i = 0; i < mPayload[id].maxPackId; i++) {
                if (mPayload[id].len[i] > 0) {
                    if (i == (mPayload[id].maxPackId - 1)) {
                        crc = ah::crc16(mPayload[id].data[i], mPayload[id].len[i] - 2, crc);
                        crcRcv = (mPayload[id].data[i][mPayload[id].len[i] - 2] << 8) | (mPayload[id].data[i][mPayload[id].len[i] - 1]);
                    } else
                        crc = ah::crc16(mPayload[id].data[i], mPayload[id].len[i], crc);
                }
                yield();
            }

            return (crc == crcRcv) ? true : false;
        }

        void reset(uint8_t id) {
            DPRINTLN(DBG_INFO, "resetPayload: id: " + String(id));
            memset(mPayload[id].len, 0, MAX_PAYLOAD_ENTRIES);
            mPayload[id].txCmd       = 0;
            mPayload[id].gotFragment = false;
            mPayload[id].retransmits = 0;
            mPayload[id].maxPackId   = MAX_PAYLOAD_ENTRIES;
            mPayload[id].lastFound   = false;
            mPayload[id].complete    = false;
            mPayload[id].requested   = false;
            mPayload[id].ts          = *mTimestamp;
        }

        IApp *mApp;
        HMSYSTEM *mSys;
        statistics_t *mStat;
        uint8_t mMaxRetrans;
        uint32_t *mTimestamp;
        invPayload_t mPayload[MAX_NUM_INVERTERS];
        bool mSerialDebug;
        Inverter<> *mHighPrioIv;
};

#endif /*__PAYLOAD_H_*/
