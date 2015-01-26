
/*
 * Copyright (C) lcinx
 * lcinx@163.com
*/

#include <stdio.h>
#include <string.h>
#include "ossome.h"
#include "net_module.h"
#include "lxnet.h"
#include "net_buf.h"
#include "pool.h"
#include "msgbase.h"
#include "log.h"
#include "crosslib.h"
#include "lxnet_datainfo.h"

struct datainfomgr {
	bool isinit;
	int64 lasttime;
	struct datainfo datatable[enum_netdata_end];
};

static struct datainfomgr s_datamgr = {false};

struct infomgr {
	bool isinit;
	struct poolmgr *encrypt_pool;
	spin_lock_struct encrypt_lock;

	struct poolmgr *socket_pool;
	spin_lock_struct socket_lock;
	
	struct poolmgr *listen_pool;
	spin_lock_struct listen_lock;
};
static struct infomgr s_infomgr = {false};

struct encryptinfo {
	enum {
		enum_encrypt_len = 32,
	};

	int maxidx;
	int nowidx;
	char buf[enum_encrypt_len];
};

static inline void on_sendmsg(size_t len) {
	assert(s_datamgr.isinit);
	s_datamgr.datatable[enum_netdata_total].sendmsgnum += 1;
	s_datamgr.datatable[enum_netdata_total].sendbytes += len;

	s_datamgr.datatable[enum_netdata_now].sendmsgnum += 1;
	s_datamgr.datatable[enum_netdata_now].sendbytes += len;
}

static inline void on_recvmsg(size_t len) {
	assert(s_datamgr.isinit);

	s_datamgr.datatable[enum_netdata_total].recvmsgnum += 1;
	s_datamgr.datatable[enum_netdata_total].recvbytes += len;

	s_datamgr.datatable[enum_netdata_now].recvmsgnum += 1;
	s_datamgr.datatable[enum_netdata_now].recvbytes += len;
}

static void datarun() {
	assert(s_datamgr.isinit);
	int64 currenttime = get_millisecond();
	if (currenttime - s_datamgr.lasttime < 1000)
		return;

	s_datamgr.lasttime = currenttime;

	time_t curtm = time(NULL);
	if (s_datamgr.datatable[enum_netdata_max].sendmsgnum <= s_datamgr.datatable[enum_netdata_now].sendmsgnum) {
		s_datamgr.datatable[enum_netdata_max].sendmsgnum = s_datamgr.datatable[enum_netdata_now].sendmsgnum;
		s_datamgr.datatable[enum_netdata_max].tm_sendmsgnum = curtm;
	}

	if (s_datamgr.datatable[enum_netdata_max].recvmsgnum <= s_datamgr.datatable[enum_netdata_now].recvmsgnum) {
		s_datamgr.datatable[enum_netdata_max].recvmsgnum = s_datamgr.datatable[enum_netdata_now].recvmsgnum;
		s_datamgr.datatable[enum_netdata_max].tm_recvmsgnum = curtm;
	}

	if (s_datamgr.datatable[enum_netdata_max].sendbytes <= s_datamgr.datatable[enum_netdata_now].sendbytes) {
		s_datamgr.datatable[enum_netdata_max].sendbytes = s_datamgr.datatable[enum_netdata_now].sendbytes;
		s_datamgr.datatable[enum_netdata_max].tm_sendbytes = curtm;
	}

	if (s_datamgr.datatable[enum_netdata_max].recvbytes <= s_datamgr.datatable[enum_netdata_now].recvbytes) {
		s_datamgr.datatable[enum_netdata_max].recvbytes = s_datamgr.datatable[enum_netdata_now].recvbytes;
		s_datamgr.datatable[enum_netdata_max].tm_recvbytes = curtm;
	}

	s_datamgr.datatable[enum_netdata_now].sendmsgnum = 0;
	s_datamgr.datatable[enum_netdata_now].recvmsgnum = 0;
	s_datamgr.datatable[enum_netdata_now].sendbytes = 0;
	s_datamgr.datatable[enum_netdata_now].recvbytes = 0;
}

static bool infomgr_init(size_t socketnum, size_t listennum) {
	if (s_infomgr.isinit)
		return false;

	s_infomgr.encrypt_pool = poolmgr_create(sizeof(struct encryptinfo), 8, socketnum * 2, 1, "encrypt buffer pool");
	s_infomgr.socket_pool = poolmgr_create(sizeof(lxnet::Socketer), 8, socketnum, 1, "Socketer obj pool");
	s_infomgr.listen_pool = poolmgr_create(sizeof(lxnet::Listener), 8, listennum, 1, "Listen obj pool");
	if (!s_infomgr.socket_pool || !s_infomgr.encrypt_pool || !s_infomgr.listen_pool) {
		poolmgr_release(s_infomgr.socket_pool);
		poolmgr_release(s_infomgr.encrypt_pool);
		poolmgr_release(s_infomgr.listen_pool);
		return false;
	}

	spin_lock_init(&s_infomgr.encrypt_lock);
	spin_lock_init(&s_infomgr.socket_lock);
	spin_lock_init(&s_infomgr.listen_lock);
	s_infomgr.isinit = true;

	s_datamgr.isinit = true;
	s_datamgr.lasttime = 0;
	for (int i = 0; i < enum_netdata_end; ++i) {
		s_datamgr.datatable[i].sendmsgnum = 0;
		s_datamgr.datatable[i].recvmsgnum = 0;
		s_datamgr.datatable[i].sendbytes = 0;
		s_datamgr.datatable[i].recvbytes = 0;
		s_datamgr.datatable[i].tm_sendmsgnum = 0;
		s_datamgr.datatable[i].tm_recvmsgnum = 0;
		s_datamgr.datatable[i].tm_sendbytes = 0;
		s_datamgr.datatable[i].tm_recvbytes = 0;
	}
	return true;
}

static void infomgr_release() {
	if (!s_infomgr.isinit)
		return;
	s_infomgr.isinit = false;
	s_datamgr.isinit = false;
	poolmgr_release(s_infomgr.socket_pool);
	poolmgr_release(s_infomgr.encrypt_pool);
	poolmgr_release(s_infomgr.listen_pool);
	spin_lock_delete(&s_infomgr.encrypt_lock);
	spin_lock_delete(&s_infomgr.socket_lock);
	spin_lock_delete(&s_infomgr.listen_lock);
}

static void encrypt_info_release(void *info) {
	if (!s_infomgr.isinit)
		return;

	spin_lock_lock(&s_infomgr.encrypt_lock);
	poolmgr_freeobject(s_infomgr.encrypt_pool, info);
	spin_lock_unlock(&s_infomgr.encrypt_lock);
}


namespace lxnet {


/* ����*/
bool Listener::Listen(unsigned short port, int backlog) {
	return listener_listen(m_self, port, backlog);
}

/* �ر����ڼ������׽��֣�ֹͣ����*/
void Listener::Close() {
	listener_close(m_self);
}

/* �����Ƿ��ѹر�*/
bool Listener::IsClose() {
	return listener_isclose(m_self);
}

/* ��ָ���ļ���socket�Ͻ�������*/
Socketer *Listener::Accept(bool bigbuf) {
	struct socketer *sock = listener_accept(m_self, bigbuf);
	if (!sock)
		return NULL;
	spin_lock_lock(&s_infomgr.socket_lock);
	Socketer *self = (Socketer *)poolmgr_getobject(s_infomgr.socket_pool);
	spin_lock_unlock(&s_infomgr.socket_lock);
	if (!self) {
		socketer_release(sock);
		return NULL;
	}

	self->m_encrypt = NULL;
	self->m_decrypt = NULL;
	self->m_self = sock;
	return self;
}

/* ����Ƿ����µ�����*/
bool Listener::CanAccept() {
	return listener_can_accept(m_self);
}

/* ���ý��������ֽڵ��ٽ�ֵ��������ֵ����ֹͣ���գ���С�ڵ���0������Ϊ������*/
void Socketer::SetRecvCritical(long size) {
	socketer_set_recv_critical(m_self, size);
}

/* ���÷��������ֽڵ��ٽ�ֵ�������������ݳ��ȴ��ڴ�ֵ����Ͽ������ӣ���Ϊ0������Ϊ������*/
void Socketer::SetSendCritical(long size) {
	socketer_set_send_critical(m_self, size);
}

/* ���Է������������ã���������ѹ������Ҫ����ѹ������˺����ڴ���socket����󼴿̵���*/
void Socketer::UseCompress() {
	socketer_use_compress(m_self);
}

/* �����ã����Խ��յ����������ã����ý�ѹ���������Ḻ���ѹ�������������ͻ���ʹ��*/
void Socketer::UseUncompress() {
	socketer_use_uncompress(m_self);
}

/* ���ü���/���ܺ����� �Լ�������;�Ĳ������/�����߼������ݡ�
 * ������/���ܺ���ΪNULL���򱣳�Ĭ�ϡ�
 * */
void Socketer::SetEncryptDecryptFunction(void (*encryptfunc)(void *logicdata, char *buf, int len), void (*release_encrypt_logicdata)(void *), void *encrypt_logicdata, void (*decryptfunc)(void *logicdata, char *buf, int len), void (*release_decrypt_logicdata)(void *), void *decrypt_logicdata) {
	socketer_set_encrypt_function(m_self, encryptfunc, release_encrypt_logicdata, encrypt_logicdata);
	socketer_set_decrypt_function(m_self, decryptfunc, release_decrypt_logicdata, decrypt_logicdata);
}

static void encrypt_decrypt_as_key_do_func(void *logicdata, char *buf, int len) {
	struct encryptinfo *o = (struct encryptinfo *)logicdata;

	int i;
	for (i = 0; i < len; i++) {
		if (o->nowidx >= o->maxidx)
			o->nowidx = 0;

		buf[i] ^= o->buf[o->nowidx];
		o->nowidx++;
	}
}

/* ���ü���key */
void Socketer::SetEncryptKey(const char *key, int key_len) {
	if (!key || key_len < 0)
		return;

	if (!m_encrypt) {
		spin_lock_lock(&s_infomgr.encrypt_lock);
		m_encrypt = (struct encryptinfo *)poolmgr_getobject(s_infomgr.encrypt_pool);
		spin_lock_unlock(&s_infomgr.encrypt_lock);

		if (m_encrypt) {
			m_encrypt->maxidx = 0;
			m_encrypt->nowidx = 0;
			memset(m_encrypt->buf, 0, sizeof(m_encrypt->buf));
			socketer_set_encrypt_function(m_self, encrypt_decrypt_as_key_do_func, encrypt_info_release, m_encrypt);
		}
	}

	if (m_encrypt) {
		m_encrypt->maxidx = key_len > encryptinfo::enum_encrypt_len ? encryptinfo::enum_encrypt_len : key_len;
		memcpy(&m_encrypt->buf, key, m_encrypt->maxidx);
	}
}

/* ���ý���key */
void Socketer::setDecryptKey(const char *key, int key_len) {
	if (!key || key_len < 0)
		return;

	if (!m_decrypt) {
		spin_lock_lock(&s_infomgr.encrypt_lock);
		m_decrypt = (struct encryptinfo *)poolmgr_getobject(s_infomgr.encrypt_pool);
		spin_lock_unlock(&s_infomgr.encrypt_lock);

		if (m_decrypt) {
			m_decrypt->maxidx = 0;
			m_decrypt->nowidx = 0;
			memset(m_decrypt->buf, 0, sizeof(m_decrypt->buf));
			socketer_set_decrypt_function(m_self, encrypt_decrypt_as_key_do_func, encrypt_info_release, m_decrypt);
		}
	}

	if (m_decrypt) {
		m_decrypt->maxidx = key_len > encryptinfo::enum_encrypt_len ? encryptinfo::enum_encrypt_len : key_len;
		memcpy(&m_decrypt->buf, key, m_decrypt->maxidx);
	}
	
}

/* �����ü��ܣ�*/
void Socketer::UseEncrypt() {
	socketer_use_encrypt(m_self);
}

/* �����ý��ܣ�*/
void Socketer::UseDecrypt() {
	socketer_use_decrypt(m_self);
}

/* ����TGW���� */
void Socketer::UseTGW() {
	socketer_use_tgw(m_self);
}

/* �ر��������ӵ�socket����*/
void Socketer::Close() {
	socketer_close(m_self);
}

/* ����ָ���ķ�����*/
bool Socketer::Connect(const char *ip, short port) {
	return socketer_connect(m_self, ip, port);
}

/* ����socket�׽����Ƿ��ѹر�*/
bool Socketer::IsClose() {
	return socketer_isclose(m_self);
}

/* ��ȡ�˿ͻ���ip��ַ*/
void Socketer::GetIP(char *ip, size_t len) {
	socketer_getip(m_self, ip, len);
}

/* �������ݣ������ǰ�����ѹ��������У�adddataΪ���ӵ�pMsg��������ݣ���Ȼ���Զ��޸�pMsg�ĳ��ȣ�addsizeָ��adddata�ĳ���*/
bool Socketer::SendMsg(Msg *pMsg, void *adddata, size_t addsize) {
	if (!pMsg)
		return false;
	if (adddata && addsize == 0) {
		assert(false);
		return false;
	}

	if (!adddata && addsize != 0) {
		assert(false);
		return false;
	}

	if (pMsg->GetLength() <= (int)sizeof(int))
		return false;

	if (pMsg->GetLength() + addsize >= _MAX_MSG_LEN) {
		assert(false && "if (pMsg->GetLength() + addsize >= _MAX_MSG_LEN)");
		log_error("	if (pMsg->GetLength() + addsize >= _MAX_MSG_LEN)");
		return false;
	}

	if (socketer_send_islimit(m_self, pMsg->GetLength()+addsize)) {
		Close();
		return false;
	}

	on_sendmsg(pMsg->GetLength() + addsize);

	if (adddata && addsize != 0) {
		bool res1, res2;
		int onesend = pMsg->GetLength();
		pMsg->SetLength(onesend + addsize);
		res1 = socketer_sendmsg(m_self, pMsg, onesend);
		
		//�����м�Ҫ�޸Ļ�ȥ�� ��������ͬһ�����������͸�һ���б���Ȼ��ÿ�ζ�������ͬβ�͡����������龰����ô������˻ָ���
		pMsg->SetLength(onesend);
		res2 = socketer_sendmsg(m_self, adddata, addsize);
		return (res1 && res2);
	} else {
		return socketer_sendmsg(m_self, pMsg, pMsg->GetLength());
	}
}

/* ��as3���Ͳ����ļ� */
bool Socketer::SendPolicyData() {
	//as3�׽��ֲ����ļ�
	char buf[512] = "<cross-domain-policy> <allow-access-from domain=\"*\" secure=\"false\" to-ports=\"*\"/> </cross-domain-policy> ";
	size_t datasize = strlen(buf);
	if (socketer_send_islimit(m_self, datasize)) {
		Close();
		return false;
	}

	on_sendmsg(datasize + 1);
	return socketer_sendmsg(m_self, buf, datasize + 1);
}

/* ����TGW��Ϣͷ */
bool Socketer::SendTGWInfo(const char *domain, int port) {
	char buf[1024] = {};
	size_t datasize;
	snprintf(buf, sizeof(buf) - 1, "tgw_l7_forward\r\nHost: %s:%d\r\n\r\n", domain, port);
	buf[sizeof(buf) - 1] = '\0';
	datasize = strlen(buf);
	if (socketer_send_islimit(m_self, datasize)) {
		Close();
		return false;
	}
	socketer_set_raw_datasize(m_self, datasize);
	on_sendmsg(datasize);
	return socketer_sendmsg(m_self, buf, datasize);
}

/* ���������ķ�������*/
void Socketer::CheckSend() {
	socketer_checksend(m_self);
}

/* ����Ͷ�ݽ��ղ���*/
void Socketer::CheckRecv() {
	socketer_checkrecv(m_self);
}

/* ��������*/
Msg *Socketer::GetMsg(char *buf, size_t bufsize) {
	Msg *obj = (Msg *)socketer_getmsg(m_self, buf, bufsize);
	if (obj) {
		if (obj->GetLength() < (int)sizeof(Msg)) {
			Close();
			return NULL;
		}
		on_recvmsg(obj->GetLength());
	}
	return obj;
}

/* �������� */
bool Socketer::SendData(const char *data, size_t datasize) {
	if (!data)
		return false;

	if (socketer_send_islimit(m_self, datasize)) {
		Close();
		return false;
	}

	on_sendmsg(datasize);

	return socketer_sendmsg(m_self, (void *)data, datasize);
}

/* �������� */
char *Socketer::GetData(char *buf, size_t bufsize, int *datalen) {
	char *data = (char *)socketer_getdata(m_self, buf, bufsize, datalen);
	if (data) {
		on_recvmsg(*datalen);
	}

	return data;
}


/* ��ʼ�����磬
 * bigbufsizeָ�����Ĵ�С��bigbufnumָ��������Ŀ��
 * smallbufsizeָ��С��Ĵ�С��smallbufnumָ��С�����Ŀ
 * listen numָ�����ڼ������׽��ֵ���Ŀ��socket num�������ӵ�����Ŀ
 * threadnumָ�������߳���Ŀ��������ΪС�ڵ���0����Ὺ��cpu�������߳���Ŀ
 */
bool net_init(size_t bigbufsize, size_t bigbufnum, size_t smallbufsize, size_t smallbufnum,
		size_t listenernum, size_t socketnum, int threadnum) {
	if (!infomgr_init(socketnum, listenernum))
		return false;
	if (!netinit(bigbufsize, bigbufnum, smallbufsize, smallbufnum,
			listenernum, socketnum, threadnum)) {
		infomgr_release();
		return false;
	}
	return true;
}

/* ��ȡ�˽������ڵĻ�����*/
const char *GetHostName() {
	static char buf[1024*16];
	buf[0] = '\0';
	socketer_gethostname(buf, sizeof(buf) - 1);
	return buf;
}

/* ����������ȡip��ַ */
const char *GetHostIPByName(const char *hostname) {
	static char buf[128];
	buf[0] = '\0';
	socketer_gethostbyname(hostname, buf, sizeof(buf));
	return buf;
}

/* ����/���ý��ܵ����ӵ��µĴ�����־��������֮ǰ��ֵ */
bool SetEnableErrorLog(bool flag) {
	return buf_set_enable_errorlog(flag);
}

/* ��ȡ��ǰ���û���ý��ܵ����ӵ��µĴ�����־ */
bool GetEnableErrorLog() {
	return buf_get_enable_errorlog();
}

/* ����һ�����ڼ����Ķ���*/
Listener *Listener_create() {
	if (!s_infomgr.isinit) {
		assert(false && "Listener_create not init!");
		return NULL;
	}
	struct listener *ls = listener_create();
	if (!ls)
		return NULL;
	spin_lock_lock(&s_infomgr.listen_lock);
	Listener *self = (Listener *)poolmgr_getobject(s_infomgr.listen_pool);
	spin_lock_unlock(&s_infomgr.listen_lock);
	if (!self) {
		listener_release(ls);
		return NULL;
	}
	self->m_self = ls;
	return self;
}

/* �ͷ�һ�����ڼ����Ķ���*/
void Listener_release(Listener *self) {
	if (!self)
		return;
	if (self->m_self) {
		listener_release(self->m_self);
		self->m_self = NULL;
	}
	spin_lock_lock(&s_infomgr.listen_lock);
	poolmgr_freeobject(s_infomgr.listen_pool, self);
	spin_lock_unlock(&s_infomgr.listen_lock);
}

/* ����һ��Socketer����*/
Socketer *Socketer_create(bool bigbuf) {
	if (!s_infomgr.isinit) {
		assert(false && "Csocket_create not init!");
		return NULL;
	}
	struct socketer *so = socketer_create(bigbuf);
	if (!so)
		return NULL;
	spin_lock_lock(&s_infomgr.socket_lock);
	Socketer *self = (Socketer *)poolmgr_getobject(s_infomgr.socket_pool);
	spin_lock_unlock(&s_infomgr.socket_lock);
	if (!self) {
		socketer_release(so);
		return NULL;
	}

	self->m_encrypt = NULL;
	self->m_decrypt = NULL;
	self->m_self = so;
	return self;
}

/* �ͷ�Socketer���󣬻��Զ����ùرյ��ƺ����*/
void Socketer_release(Socketer *self) {
	if (!self)
		return;
	
	if (self->m_self) {
		socketer_release(self->m_self);
		self->m_self = NULL;
	}

	self->m_encrypt = NULL;
	self->m_decrypt = NULL;

	spin_lock_lock(&s_infomgr.socket_lock);
	poolmgr_freeobject(s_infomgr.socket_pool, self);
	spin_lock_unlock(&s_infomgr.socket_lock);
}

/* �ͷ��������*/
void net_release() {
	infomgr_release();
	netrelease();
}

/* ִ����ز�������Ҫ�����߼��е��ô˺���*/
void net_run() {
	datarun();
	netrun();
}

static char s_memory_info[1024*64];

/* ��ȡsocket����أ�listen����أ����أ�С��ص�ʹ�����*/
const char *net_memory_info() {
	size_t index = 0;

	snprintf(&s_memory_info[index], sizeof(s_memory_info)-1-index, "%s", "lxnet lib memory pool info:\n<+++++++++++++++++++++++++++++++++++++++++++++++++++++>");
	index = strlen(s_memory_info);

	spin_lock_lock(&s_infomgr.encrypt_lock);
	poolmgr_getinfo(s_infomgr.encrypt_pool, &s_memory_info[index], sizeof(s_memory_info)-1-index);
	spin_lock_unlock(&s_infomgr.encrypt_lock);

	index = strlen(s_memory_info);

	spin_lock_lock(&s_infomgr.socket_lock);
	poolmgr_getinfo(s_infomgr.socket_pool, &s_memory_info[index], sizeof(s_memory_info)-1-index);
	spin_lock_unlock(&s_infomgr.socket_lock);
	
	index = strlen(s_memory_info);

	spin_lock_lock(&s_infomgr.listen_lock);
	poolmgr_getinfo(s_infomgr.listen_pool, &s_memory_info[index], sizeof(s_memory_info)-1-index);
	spin_lock_unlock(&s_infomgr.listen_lock);

	index = strlen(s_memory_info);
	netmemory_info(&s_memory_info[index], sizeof(s_memory_info)-1-index);

	index = strlen(s_memory_info);
	snprintf(&s_memory_info[index], sizeof(s_memory_info)-1-index, "%s", "<+++++++++++++++++++++++++++++++++++++++++++++++++++++>");
	return s_memory_info;
}

//��ȡ��ǰʱ�䡣��ʽΪ"2010-09-16 23:20:20"
static const char *CurrentTimeStr(time_t tval, char *buf, size_t buflen) {
	if (buflen < 64)
		return "null";

	struct tm tm_result;
	struct tm *currTM = safe_localtime(&tval, &tm_result);
	snprintf(buf, buflen-1, "%d-%02d-%02d %02d:%02d:%02d", currTM->tm_year+1900, currTM->tm_mon+1, currTM->tm_mday, currTM->tm_hour, currTM->tm_min, currTM->tm_sec);
	buf[buflen-1] = '\0';
	return buf;
}

/* ��ȡ�����ͨѶ����*/
const char *net_datainfo() {
	static char infostr[1024*8];
	if (!s_datamgr.isinit)
		return "not init!";

	double numunit = 1000*1000;
	double bytesunit = 1024*1024;
	double totalsendmsgnum = (double)(s_datamgr.datatable[enum_netdata_total].sendmsgnum / numunit);
	double totalsendbytes = double(s_datamgr.datatable[enum_netdata_total].sendbytes / bytesunit);
	double totalrecvmsgnum = (double)(s_datamgr.datatable[enum_netdata_total].recvmsgnum / numunit);
	double totalrecvbytes = double(s_datamgr.datatable[enum_netdata_total].recvbytes / bytesunit);

	double maxsendmsgnum = (double)(s_datamgr.datatable[enum_netdata_max].sendmsgnum);
	double maxsendbytes = double(s_datamgr.datatable[enum_netdata_max].sendbytes / bytesunit);
	double maxrecvmsgnum = (double)(s_datamgr.datatable[enum_netdata_max].recvmsgnum);
	double maxrecvbytes = double(s_datamgr.datatable[enum_netdata_max].recvbytes / bytesunit);

	double nowsendmsgnum = (double)(s_datamgr.datatable[enum_netdata_now].sendmsgnum);
	double nowsendbytes = double(s_datamgr.datatable[enum_netdata_now].sendbytes / bytesunit);
	double nowrecvmsgnum = (double)(s_datamgr.datatable[enum_netdata_now].recvmsgnum);
	double nowrecvbytes = double(s_datamgr.datatable[enum_netdata_now].recvbytes / bytesunit);

	char buf_sendmsgnum[128] = {};
	char buf_sendbytes[128] = {};
	char buf_recvmsgnum[128] = {};
	char buf_recvbytes[128] = {};
	CurrentTimeStr(s_datamgr.datatable[enum_netdata_max].tm_sendmsgnum, buf_sendmsgnum, sizeof(buf_sendmsgnum));
	CurrentTimeStr(s_datamgr.datatable[enum_netdata_max].tm_sendbytes, buf_sendbytes, sizeof(buf_sendbytes));
	CurrentTimeStr(s_datamgr.datatable[enum_netdata_max].tm_recvmsgnum, buf_recvmsgnum, sizeof(buf_recvmsgnum));
	CurrentTimeStr(s_datamgr.datatable[enum_netdata_max].tm_recvbytes, buf_recvbytes, sizeof(buf_recvbytes));

	snprintf(infostr, sizeof(infostr) - 1, "total: send msg num:%lfM, send bytes:%lfMB, recv msg num:%lfM, recv bytes:%lfMB\nmax:\nsend msg num:%lf, time:%s\nsend bytes:%lfMB, time:%s\nrecv msg num:%lf, time:%s\nrecv bytes:%lfMB, time:%s\nnow: send msg num:%lf, send bytes:%lfMB, recv msg num:%lf, recv bytes:%lfMB\n", totalsendmsgnum, totalsendbytes, totalrecvmsgnum, totalrecvbytes, maxsendmsgnum, buf_sendmsgnum, maxsendbytes, buf_sendbytes, maxrecvmsgnum, buf_recvmsgnum, maxrecvbytes, buf_recvbytes, nowsendmsgnum, nowsendbytes, nowrecvmsgnum, nowrecvbytes);

	return infostr;
}

/* ��ȡ������ָ�����͵�����*/
struct datainfo *net_datainfo_bytype(int type) {
	if (type < 0 || type >= (int)enum_netdata_end)
		return NULL;

	return &s_datamgr.datatable[type];
}

}

