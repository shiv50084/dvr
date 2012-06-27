
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "eaglesvr.h"
#include "draw.h"

dvrsvr *dvrsvr::m_head = NULL;

dvrsvr::dvrsvr(int fd)
{
    pthread_mutex_init(&m_mutex, NULL);
    
    m_sockfd = fd;
    m_shutdown = 0 ;
    m_fifo = NULL;
    m_recvbuf = NULL;
    m_recvlen = 0;
    m_recvloc = 0;
    m_req.reqcode = 0;
    m_req.reqsize = 0;
    m_conntype = CONN_NORMAL;
    m_fifosize=0;

    m_fontcache=NULL ;
    // add into svr queue
    m_next = m_head;
    m_head = this;

}

dvrsvr::~dvrsvr()
{
    lock();

    // remove from dvr list
    if (m_head == this) {
        m_head = m_next;
    } else {
        dvrsvr *walk = m_head;
        while (walk->m_next != NULL) {
            if (walk->m_next == this) {
                walk->m_next = this->m_next;
                break;
            }
            walk = walk->m_next;
        }
    }

    m_shutdown=0 ;

    if( m_sockfd>0) {
        net_close(m_sockfd);
    }
    
    cleanfifo();
    if( m_recvbuf ) {
        mem_free( m_recvbuf ) ;
    }
    if( m_fontcache!=NULL ) {
        mem_free( m_fontcache );
    }
    unlock();

    pthread_mutex_destroy(&m_mutex);
}

// return 0: no data, 1: may have more data
int dvrsvr::read()
{
    int n;
    char *preq;
    if( isdown() ) {
        return 0;
    }
    if (m_recvbuf == NULL) {	// receiving req struct
        preq = (char *) &m_req;
        if (m_recvlen == 0) {
            m_recvlen = sizeof(m_req);
            m_recvloc = 0;
        }
        n = ::recv(m_sockfd, &preq[m_recvloc], m_recvlen - m_recvloc, 0);
        if (n == 0) {			// error or connection closed by peer
            down() ;            // indicate socket is down
            return 0 ;
        }
        else if( n<0 ) {
            if( errno!=EWOULDBLOCK ) {
                down() ;        // indicate socket is down
            }
            return 0 ;
        }
        m_recvloc += n;
        if (m_recvloc < m_recvlen)
            return 1 ;
        
        m_recvlen = 0;
        if (m_req.reqsize > 0x100000 || m_req.reqsize < 0) {	// invalid req
            m_req.reqcode = 0;
            m_req.reqsize = 0;
            return 0 ;
        }
        else if (m_req.reqsize>0 ){	// wait for extra data
            m_recvbuf = (char *)mem_alloc(m_req.reqsize);
            if (m_recvbuf == NULL) {
                // no enough memory
                down();             // indicate socket is down
                return 0 ;
            }
            else {
                m_recvlen = m_req.reqsize;
                m_recvloc = 0;
                return 1;
            }
        }
    } else {
        n = ::recv(m_sockfd, &m_recvbuf[m_recvloc], m_recvlen - m_recvloc, 0);
        if (n == 0) {			// connection closed by peer
            down();             // indicate socket is down
            return 0 ;
        }
        else if( n<0 ) {
            if( errno!=EWOULDBLOCK ) {		// error!
                down();         // mark socket down
            }
            return 0 ;
        }
        m_recvloc += n;
        if (m_recvloc < m_recvlen)	// buffer not complete
        {
            return 1 ;
        }
    }
    
    // req completed, process it
    onrequest();

    if (m_recvbuf != NULL) {
        mem_free(m_recvbuf);
        m_recvbuf = NULL;
    }
    m_recvlen = m_recvloc = 0 ;
    m_req.reqcode = 0;
    m_req.reqsize = 0;
    return 0 ;                      // don't continue
}

void dvrsvr::cleanfifo()
{
    net_fifo * nfifo ;
    while( m_fifo ) {
        if (m_fifo->buf != NULL) {
            mem_free(m_fifo->buf);
        }
        nfifo=m_fifo->next ;
        delete m_fifo ;
        m_fifo = nfifo;
    }
    m_fifosize=0;
}

// clean fifo except current transmiting one
void dvrsvr::cleanfifotail()
{
    if( m_fifo ) {
        net_fifo * nfifo = m_fifo->next ;
        m_fifo->next = NULL ;
        while( nfifo ) {
            net_fifo * nxfifo = nfifo->next ;
            if (nfifo->buf != NULL) {
                mem_free(nfifo->buf);
            }
            delete nfifo ;
            nfifo=nxfifo ;
        }
    }
}

// send out data from fifo
// return 0: no more data
//        1: more data in fifo
//       -1: error
int dvrsvr::write()
{
    int res=0;
    int n;
    net_fifo * nfifo ;

    if( isdown() ) {
        return 0;
    }

    lock();
    while( m_fifo ) {
        if (m_fifo->buf == NULL) {
            nfifo=m_fifo->next ;
            delete m_fifo ;
            m_fifo = nfifo;
        }
        else if(m_fifo->loc >= m_fifo->bufsize) {
            nfifo=m_fifo->next ;
            mem_free(m_fifo->buf);
            delete m_fifo ;
            m_fifo = nfifo;
        }
        else {
            n =::send(m_sockfd,
                      &(m_fifo->buf[m_fifo->loc]),
                      m_fifo->bufsize - m_fifo->loc, 0);
            if( n<0 ) {
                if(errno == EWOULDBLOCK ) {
                    res = 1 ;
                    break ;
                }
                else {
                    res = -1 ;          // other error!
                    down();             // mark socket down
                    break;
                }
            }
            m_fifosize -= n ;
            m_fifo->loc += n;
        }
    }
    if( m_fifo==NULL ) {
        m_fifosize=0;
        res=0 ;
    }
    unlock();
    return res ;
}

void dvrsvr::send_fifo(char *buf, int bufsize, int loc)
{
    net_fifo *nfifo;
    
    nfifo = new net_fifo ;
    nfifo->buf = (char *)mem_addref( buf, bufsize );
    nfifo->bufsize = bufsize;
    nfifo->loc = loc;
    nfifo->next = NULL;
    
    if (m_fifo == NULL) {           // current fifo is empty
        m_fifo = m_fifotail = nfifo;
        m_fifosize = bufsize ;
        net_trigger();
    } else {
        m_fifotail->next = nfifo ;
        m_fifotail = nfifo ;
        m_fifosize += bufsize ;
    }
}

void dvrsvr::Send(void *buf, int bufsize)
{
    if( isdown() ) {
        return ;
    }
    
    lock();
    if ( m_fifo ) {
        send_fifo((char *)buf, bufsize);
    }
    else {
        int loc = 0 ;
        int n ;
        while( loc<bufsize ) {
            n = ::send(m_sockfd, ((char *)buf)+loc, bufsize-loc, 0);
            if( n<0 ) {
                if( errno==EWOULDBLOCK ) {
                    send_fifo(((char *)buf), bufsize, loc);
                }
                else {
                    // network error or peer closed socket!
                    down();         // mark socket down
                }
                break ;
            }
            loc+=n ;
        }
    }
    unlock();
}

int dvrsvr::onframe(cap_frame * pframe)
{
    struct dvr_ans ans ;

    if( isdown() ) {
        return 0;
    }
    if( m_connchannel == pframe->channel ) {
        if (m_conntype == CONN_LIVESTREAM ) {
            if( m_fifo && m_fifo->next ) {		// only allow two fifos (one frame)
                m_fifodrop=1 ;
            }
            else if ( m_fifodrop==0 || pframe->frametype == FRAMETYPE_KEYVIDEO ) {
                m_fifodrop=0;
                ans.anscode = ANSSTREAMDATA ;
                ans.anssize = pframe->framesize ;
                ans.data = pframe->frametype ;
                Send(&ans, sizeof(struct dvr_ans));
                Send(pframe->framedata, pframe->framesize);
            }
            return 1;
        }
        else if (m_conntype == CONN_REALTIME ) {
            if ( pframe->frametype == FRAMETYPE_KEYVIDEO ){	// for keyframes
                // clean all tailing fifo
                cleanfifotail();
            }
            Send(pframe->framedata, pframe->framesize);
            return 1;
        }
        else if ( pframe->frametype==FRAMETYPE_JPEG ) {
            ans.anscode = ANS2JPEG ;
            ans.anssize = pframe->framesize ;
            ans.data = pframe->frametype ;
            Send(&ans, sizeof(struct dvr_ans));
            Send(pframe->framedata, pframe->framesize);
            return 1;
        }
    }
    return 0;
}

void dvrsvr::onrequest()
{
    switch (m_req.reqcode) {
    case REQOK:
        AnsOk();
        break ;
    case REQREALTIME:
        ReqRealTime();
        break;
    case REQCHANNELINFO:
        ChannelInfo();
        break;
    case REQSERVERNAME:
        DvrServerName();
        break;
    case REQHOSTNAME:
        HostName();
        break;
    case REQGETSYSTEMSETUP:
        GetSystemSetup();
        break;
    case REQGETCHANNELSETUP:
        GetChannelSetup();
        break;
    case REQSETCHANNELSETUP:
        SetChannelSetup();
        break;
    case REQGETCHANNELSTATE:
        GetChannelState();
        break;
    case REQGETVERSION:
        GetVersion();
        break;
    case REQOPENLIVE:
        ReqOpenLive();
        break;
    case REQ2ADJTIME:
        Req2AdjTime();
        break;
    case REQ2GETLOCALTIME:
        Req2GetLocalTime();
        break;
    case REQ2SETSYSTEMTIME:
        Req2SetSystemTime();
        break;
    case REQ2GETSYSTEMTIME:
        Req2GetSystemTime();
        break;
    case REQ2SETTIMEZONE:
        Req2SetTimeZone();
        break;
    case REQSETHIKOSD:
        ReqSetHikOSD();
        break;
    case REQ2GETCHSTATE:
        Req2GetChState();
        break;
    case REQ2GETJPEG:
        Req2GetJPEG();
        break;
    case REQECHO:
        ReqEcho();
        break;

    case REQSCREEENSETMODE:
        ReqScreenSetMode();
        break;
    case REQSCREEENLIVE:
        ReqScreenLive();
        break;
    case REQSCREENPLAY:
        ReqScreenPlay();
        break;
    case REQSRCEENINPUT:
        ReqScreenInput();
        break;
    case REQSCREENSTOP:
        ReqScreenStop();
        break;
    case REQSCREENAUDIO:
        ReqScreenAudio();
        break;

    case REQGETSCREENWIDTH:
        ReqGetScreenWidth();
        break;
    case REQGETSCREENHEIGHT:
        ReqGetScreenHeight();
        break;
    case REQDRAWSETAREA:
        ReqDrawSetArea();
        break;
    case REQDRAWREFRESH:
        ReqDrawRefresh();
        break;
    case REQDRAWSETCOLOR:
        ReqDrawSetColor();
        break;
    case REQDRAWGETCOLOR:
        ReqDrawSetColor();
        break;
    case REQDRAWSETPIXELMODE:
        ReqDrawSetPixelMode();
        break;
    case REQDRAWGETPIXELMODE:
        ReqDrawGetPixelMode();
        break;
    case REQDRAWPUTPIXEL:
        ReqDrawPutPixel();
        break;
    case REQDRAWGETPIXEL:
        ReqDrawGetPixel();
        break;
    case REQDRAWLINE:
        ReqDrawLine();
        break;
    case REQDRAWRECT:
        ReqDrawRect();
        break;
    case REQDRAWFILL:
        ReqDrawFill();
        break;
    case REQDRAWCIRCLE:
        ReqDrawCircle();
        break;
    case REQDRAWFILLCIRCLE:
        ReqDrawFillCircle();
        break;
    case REQDRAWBITMAP:
        ReqDrawBitmap();
        break;
    case REQDRAWSTRETCHBITMAP:
        ReqDrawStretchBitmap();
        break;
    case REQDRAWREADBITMAP:
        ReqDrawReadBitmap();
        break;
    case REQDRAWSETFONT:
        ReqDrawSetFont();
        break;
    case REQDRAWTEXT:
        ReqDrawText();
        break;
    case REQDRAWTEXTEX:
        ReqDrawTextEx();
        break;
    default:
        AnsError();
        break;
    }
}


void dvrsvr::AnsOk()
{
    struct dvr_ans ans ;
    ans.anscode=ANSOK ;
    ans.data=0;
    ans.anssize=0;
    Send(&ans, sizeof(ans));
}

void dvrsvr::AnsError()
{
    struct dvr_ans ans ;
    ans.anscode = ANSERROR;
    ans.anssize = 0;
    ans.data = 0;
    Send(&ans, sizeof(ans));
}

void dvrsvr::ReqEcho()
{
    struct dvr_ans ans ;
    ans.anscode=ANSECHO ;
    ans.data=0;
    ans.anssize=m_recvlen ;
    Send(&ans, sizeof(ans));
    if( m_recvlen>0 ) {
        Send(m_recvbuf, m_recvlen);
    }
}

void dvrsvr::ReqRealTime()
{
    struct dvr_ans ans ;
    if (m_req.data >= 0 && m_req.data < cap_channels) {
        ans.anscode = ANSREALTIMEHEADER;
        ans.data = m_req.data;
        ans.anssize = sizeof(struct hd_file);
        Send(&ans, sizeof(ans));
        Send(cap_fileheader(m_req.data), sizeof(struct hd_file));
        m_conntype = CONN_REALTIME;
        m_connchannel = m_req.data;
        cap_channel[m_connchannel]->start();

        dvr_log( "Open Real Time channel %d", m_connchannel );

    }
    else {
        AnsError();
    }
}

struct channel_info {
    int Enable ;
    int Resolution ;
    char CameraName[64] ;
} ;

void dvrsvr::ChannelInfo()
{
    int i;
    struct dvr_ans ans ;
    struct channel_info chinfo ;
    ans.anscode = ANSCHANNELDATA ;
    ans.data = cap_channels ;
    ans.anssize = cap_channels * sizeof(struct channel_info);
    Send( &ans, sizeof(ans));
    for( i=0; i<cap_channels; i++ ) {
        memset( &chinfo, 0, sizeof(chinfo));
        chinfo.Enable=cap_channel[i]->enabled() ;
        chinfo.Resolution=0 ;
        strncpy( chinfo.CameraName, cap_channel[i]->getname(), 64);
        Send(&chinfo, sizeof(chinfo));
    }
}

void dvrsvr::HostName()
{
    struct dvr_ans ans;
    ans.anscode = ANSSERVERNAME;
    ans.anssize = strlen(g_servername)+1;
    ans.data = 0;
    Send( &ans, sizeof(ans));
    Send( (char *)g_servername, ans.anssize );
}

void dvrsvr::DvrServerName()
{
    HostName();
}

void dvrsvr::GetSystemSetup()
{
    struct dvr_ans ans ;
    struct system_stru sys ;
    memset( &sys, 0, sizeof(sys));
    if( dvr_getsystemsetup(&sys) ) {
        ans.anscode = ANSSYSTEMSETUP;
        ans.anssize = sizeof( struct system_stru );
        ans.data = 0;
        Send( &ans, sizeof(ans) );
        Send( &sys, sizeof(sys) );
    }
    else {
        AnsError ();
    }
}

void dvrsvr::GetChannelSetup()
{
    struct DvrChannel_attr dattr ;
    struct dvr_ans ans ;
    if( dvr_getchannelsetup(m_req.data, &dattr, sizeof(dattr)) ) {
        ans.anscode = ANSCHANNELSETUP;
        ans.anssize = sizeof( struct DvrChannel_attr );
        ans.data = m_req.data;
        Send( &ans, sizeof(ans));
        Send( &dattr, sizeof(dattr));
    }
    else {
        AnsError();
    }
}

void dvrsvr::SetChannelSetup()
{
    struct dvr_ans ans ;
    struct DvrChannel_attr * pchannel = (struct DvrChannel_attr *) m_recvbuf ;
    if( dvr_setchannelsetup(m_req.data, pchannel, m_req.reqsize) ) {
        ans.anscode = ANSOK;
        ans.anssize = 0;
        ans.data = 0;
        Send( &ans, sizeof(ans) );
        return ;
    }
    AnsError();
}

void dvrsvr::GetChannelState()
{
    int i ;
    struct channelstate {
        int sig ;
        int rec ;
        int mot ;
    } cs ;
    struct dvr_ans ans ;
    
    ans.anscode = ANSGETCHANNELSTATE ;
    ans.anssize = sizeof( struct channelstate ) * cap_channels ;
    ans.data = cap_channels ;
    Send( &ans, sizeof(ans));
    for( i=0; i<cap_channels; i++ ) {
        if( cap_channel[i]!=NULL ) {
            cs.sig = ( cap_channel[i]->getsignal()==0 ) ;       // signal lost?
            cs.rec =  0;                                        // channel recording?
            cs.mot = cap_channel[i]->getmotion() ;              // motion detections
        }
        else {
            cs.sig = 0 ;
            cs.rec = 0 ;
            cs.mot = 0 ;
        }
        Send(&cs, sizeof(cs));
    }
}

void dvrsvr::GetVersion()
{
    int version ;
    struct dvr_ans ans;
    ans.anscode = ANSGETVERSION;
    ans.anssize = 4*sizeof(int);
    ans.data = 0;
    Send(&ans, sizeof(ans));
    version = DVRVERSION0 ;
    Send(&version, sizeof(version));
    version = DVRVERSION1 ;
    Send(&version, sizeof(version));
    version = DVRVERSION2 ;
    Send(&version, sizeof(version));
    version = DVRVERSION3 ;
    Send(&version, sizeof(version));
}

struct ptz_cmd {
    DWORD command ;
    DWORD param ;
} ;


// Open live stream (2nd vesion)
//  m_req.data is stream channel
void dvrsvr::ReqOpenLive()
{
    struct dvr_ans ans ;
    int hlen ;

    if( m_req.data>=0 && m_req.data<cap_channels ) {
        ans.anscode = ANSSTREAMOPEN;
        ans.anssize = 0;
        ans.data = 0 ;
        Send( &ans, sizeof(ans) );
        hlen = cap_channel[m_req.data]->getheaderlen();
        if( hlen>0 ) {
            ans.anscode = ANSSTREAMDATA;
            ans.anssize = sizeof(struct hd_file) ;
            ans.data = FRAMETYPE_264FILEHEADER ;
            Send( &ans, sizeof(ans));
            Send( cap_channel[m_req.data]->getheader(), hlen );
        }
        m_conntype = CONN_LIVESTREAM;
        m_connchannel = m_req.data;
        cap_channel[m_connchannel]->start();

        dvr_log( "Open Live channel %d", m_connchannel );

        return ;
    }
    AnsError();
}

void dvrsvr::Req2AdjTime()
{
    struct dvr_ans ans ;
    if( m_recvlen>=(int)sizeof(struct dvrtime) ) {
        time_adjtime((struct dvrtime *)m_recvbuf);
        ans.anscode = ANSOK;
        ans.anssize = 0;
        ans.data = 0;
        Send( &ans, sizeof(ans));
    }
    else {
        AnsError();
    }
    return ;
}

void dvrsvr::Req2SetLocalTime()
{
    struct dvr_ans ans ;
    if( m_recvlen>=(int)sizeof(struct dvrtime) ) {
        time_setlocaltime((struct dvrtime *)m_recvbuf);
        ans.anscode = ANSOK;
        ans.anssize = 0;
        ans.data = 0;
        Send( &ans, sizeof(ans));
    }
    else {
        AnsError();
    }
}

void dvrsvr::Req2GetLocalTime()
{
    struct dvr_ans ans ;
    struct dvrtime dvrt ;
    time_now(&dvrt);
    ans.anscode = ANS2TIME;
    ans.anssize = sizeof( struct dvrtime );
    ans.data = 0 ;
    Send( &ans, sizeof(ans));
    Send( &dvrt, sizeof(dvrt));
    return ;
}

void dvrsvr::Req2SetSystemTime()
{
    struct dvr_ans ans ;
    time_setutctime((struct dvrtime *)m_recvbuf);
    ans.anscode = ANSOK;
    ans.anssize = 0;
    ans.data = 0;
    Send( &ans, sizeof(ans));
    return ;
}

void dvrsvr::Req2GetSystemTime()
{
    struct dvr_ans ans ;
    struct dvrtime dvrt ;
    ans.anscode = ANS2TIME;
    ans.anssize = sizeof( struct dvrtime );
    ans.data = 0 ;
    Send( &ans, sizeof(ans));
    time_utctime( &dvrt );
    Send( &dvrt, sizeof(dvrt));
    return ;
}

void dvrsvr::Req2SetTimeZone()
{
    struct dvr_ans ans ;
    if( m_recvbuf==NULL || m_recvlen<3 ) {
        AnsError();
        return ;
    }
    ans.anscode = ANSOK;
    ans.anssize = 0 ;
    ans.data = 0 ;
    Send( &ans, sizeof(ans));
    time_settimezone(m_recvbuf);
    return ;
}

void dvrsvr::ReqSetHikOSD()
{
    struct dvr_ans ans ;
    if( m_req.data>=0 && m_req.data<cap_channels
        && m_req.reqsize>=(int)sizeof( hik_osd_type )
        && cap_channel[m_req.data]->type() == CAP_TYPE_HIKLOCAL )
    {
        capture * pcap = (capture *)cap_channel[m_req.data] ;
        struct hik_osd_type * posd = (struct hik_osd_type *) m_recvbuf ;
        pcap->setremoteosd();
        pcap->setosd(posd);
        ans.anscode = ANSOK ;
        ans.anssize = 0 ;
        ans.data = 0 ;
        Send( &ans, sizeof(ans));
        return ;
    }
    AnsError();
}

void dvrsvr::Req2GetChState()
{
    struct dvr_ans ans ;
    int ch = m_req.data ;
    if( ch>=0 && ch<cap_channels ) {
        ans.anscode = ANS2CHSTATE ;
        ans.anssize = 0 ;
        ans.data = 0 ;
        if( cap_channel[ch]->getsignal() ) {					// bit 0: signal ok.
            ans.data |= 1 ;
        }
        if( cap_channel[ch]->getmotion() ) {					// bit 1: motion detected
            ans.data |= 2 ;
        }
        Send( &ans, sizeof(ans) );
    }
    else {
        AnsError();
    }
}

void dvrsvr::Req2GetStreamBytes()
{
    struct dvr_ans ans ;
    if( m_req.data >=0 && m_req.data < cap_channels ) {
        ans.data = cap_channel[m_req.data]->streambytes() ;
        ans.anssize=0;
        ans.anscode = ANS2STREAMBYTES ;
        Send( &ans, sizeof(ans));
        return ;
    }
    AnsError();
}

void dvrsvr::Req2GetJPEG()
{
    int ch, jpeg_quality, jpeg_pic ;
    ch = m_req.data & 0xff  ;
    jpeg_quality = (m_req.data>>8) & 0xff  ;        //0-best, 1-better, 2-average
    jpeg_pic = (m_req.data>>16) & 0xff  ;           // 0-cif , 1-qcif, 2-4cif
    if( ch >=0 && ch < cap_channels ) {
        m_connchannel = ch ;
        cap_channel[ch]->captureJPEG(jpeg_quality, jpeg_pic) ;
        return ;
    }
    AnsError();
}

// EagleSVR supports      

void dvrsvr::ReqScreenSetMode()
{
    int i;
    int * param ;
    int videostd ;
    int screennum ;
    unsigned char displayorder[16] ;
    struct dvr_ans ans ;
    videostd = 1 ;		// default NTSC
    screennum = (int)m_req.data ;
    param = (int *)m_recvbuf ;
    if( m_req.reqsize>=(int)sizeof(int) ) {
        videostd = *param ;
        param++;
        m_req.reqsize-=sizeof(int);
    }
    for( i=0 ;i<16; i++ ) {
        if( i<m_req.reqsize/(int)sizeof(int) ) {
            displayorder[i] = param[i] ;
        }
        else {
            displayorder[i] = i ;
        }
    }
    screen_setmode( videostd, screennum, displayorder );
    ans.anscode = ANSOK;
    ans.anssize = 0;
    ans.data = 0;
    Send(&ans, sizeof(ans));
}


void dvrsvr::ReqScreenLive()
{
    screen_live( m_req.data );
    AnsOk();
}

void dvrsvr::ReqScreenPlay()
{
    int speed ;
    int * param = (int *)m_recvbuf ;
    if( m_req.reqsize>=(int)sizeof(int) ) {
        speed = param[0] ;
    }
    else {
        speed = 4 ;		// normal speed
    }
    screen_playback( m_req.data, speed );
    AnsOk();
}

void dvrsvr::ReqScreenInput()
{
    screen_playbackinput( m_req.data, m_recvbuf, m_req.reqsize );
    //  no response for input data, to improve performance
    //	AnsOk();
}

void dvrsvr:: ReqScreenStop()
{
    AnsOk();
    screen_stop( m_req.data );
}

void dvrsvr::ReqScreenAudio()
{
    AnsOk();
    int audioon ;
    int * param = (int *)m_recvbuf ;
    if( m_req.reqsize>=(int)sizeof(int) ) {
        audioon = param[0] ;
    }
    else {
        audioon = 1 ;
    }
    screen_audio( m_req.data, audioon );
}

void dvrsvr::ReqGetScreenWidth()
{
    struct dvr_ans ans ;
    ans.anscode = ANSGETSCREENWIDTH ;
    ans.anssize = 0;
    ans.data = draw_screenwidth() ;
    Send(&ans, sizeof(ans));
}

void dvrsvr::ReqGetScreenHeight()
{
    struct dvr_ans ans ;
    ans.anscode = ANSGETSCREENHEIGHT ;
    ans.anssize = 0;
    ans.data = draw_screenheight() ;
    Send(&ans, sizeof(ans));
}

void dvrsvr::ReqDrawSetArea()
{
    if( m_req.reqsize>=4*(int)sizeof(int) ) {
        AnsOk();
        int * param = (int *)m_recvbuf ;
        draw_setdrawarea( param[0], param[1], param[2], param[3] );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawRefresh()
{
    AnsOk();
    draw_refresh();
}

void dvrsvr::ReqDrawSetPixelMode()
{
    AnsOk();
    draw_setpixelmode( m_req.data );
}

void dvrsvr::ReqDrawGetPixelMode()
{
    struct dvr_ans ans ;
    ans.anscode = ANSDRAWGETPIXELMODE ;
    ans.anssize = 0;
    ans.data = draw_getpixelmode() ;
    Send(&ans, sizeof(ans));
}

void dvrsvr::ReqDrawSetColor()
{
    AnsOk();
    draw_setcolor( (UINT32)m_req.data );
}

void dvrsvr::ReqDrawGetColor()
{
    struct dvr_ans ans ;
    ans.anscode = ANSDRAWGETCOLOR ;
    ans.anssize = 0;
    ans.data = draw_getcolor() ;
    Send(&ans, sizeof(ans));
}

void dvrsvr::ReqDrawPutPixel()
{
    if( m_req.reqsize>=2*(int)sizeof(int) ) {
        AnsOk();
        int * param = (int *)m_recvbuf ;
#ifdef EAGLE34
        draw_putpixel_eagle34( param[0], param[1], m_req.data );
#else		
        draw_putpixel( param[0], param[1], m_req.data );
#endif
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawGetPixel()
{
    struct dvr_ans ans ;
    if( m_req.reqsize>=2*(int)sizeof(int) ) {
        int * param = (int *)m_recvbuf ;
        ans.anscode = ANSDRAWGETPIXEL ;
        ans.anssize = 0;
        ans.data=draw_getpixel( param[0], param[1] );
        Send(&ans, sizeof(ans));
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawLine()
{
    if( m_req.reqsize>=4*(int)sizeof(int) ) {
        AnsOk();
        int * param = (int *)m_recvbuf ;
        draw_line( param[0], param[1], param[2], param[3] );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawRect()
{
    if( m_req.reqsize>=4*(int)sizeof(int) ) {
        AnsOk();
        int * param = (int *)m_recvbuf ;
        draw_rect( param[0], param[1], param[2], param[3] );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawFill()
{
    if( m_req.reqsize>=4*(int)sizeof(int) ) {
        AnsOk();
        int * param = (int *)m_recvbuf ;
        draw_fillrect( param[0], param[1], param[2], param[3] );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawCircle()
{
    if( m_req.reqsize>=3*(int)sizeof(int) ) {
        AnsOk();
        int * param = (int *)m_recvbuf ;
        draw_circle( param[0], param[1], param[2] );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawFillCircle()
{
    if( m_req.reqsize>=3*(int)sizeof(int) ) {
        AnsOk();
        int * param = (int *)m_recvbuf ;
        draw_fillcircle( param[0], param[1], param[2] );
    }
    else {
        AnsError();
    }
}

// Draw Bitmap
// BITMAP field bits are not used when bitmap transmit through network
void dvrsvr::ReqDrawBitmap()
{
    if( m_req.reqsize>11*(int)sizeof(int) ) {		// 6 parameters and BITMAP
        AnsOk();
        int * param = (int *)m_recvbuf ;
        struct BITMAP * pbmp = (struct BITMAP *)&param[6] ;
        pbmp->bits = ((UINT8 *)pbmp)+sizeof(BITMAP) ;
        draw_bitmap( pbmp, param[0], param[1], param[2], param[3], param[4], param[5] );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawStretchBitmap()
{
    if( m_req.reqsize>13*(int)sizeof(int) ) { 	// 8 parameters and BITMAP
        AnsOk();
        int * param = (int *)m_recvbuf ;
        struct BITMAP * pbmp = (struct BITMAP *)&param[8] ;
        pbmp->bits = ((UINT8 *)pbmp)+sizeof(BITMAP) ;		// replace bits
        draw_stretchbitmap( pbmp, param[0], param[1], param[2], param[3], param[4], param[5], param[6], param[7] );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawReadBitmap()
{
    if( m_req.reqsize>=4*(int)sizeof(int) ) {
        struct BITMAP bmp ;
        int * param = (int *)m_recvbuf ;
        if( draw_readbitmap( &bmp, param[0], param[1], param[2], param[3] ) ) {
            struct dvr_ans ans ;
            ans.anscode = ANSDRAWREADBITMAP ;
            ans.anssize = sizeof(bmp)+bmp.bytes_per_line * bmp.height ;
            ans.data = 0 ;
            Send( &ans, sizeof(ans) );
            Send( &bmp, sizeof(bmp) );
            Send( bmp.bits, bmp.bytes_per_line * bmp.height );
            draw_closebmp( &bmp );
            return ;
        }
    }
    AnsError();
}

void dvrsvr::ReqDrawSetFont()
{
    if( m_req.reqsize==0 && m_req.data==0 ) {	// remove font
        AnsOk();
        if(m_fontcache ) {
            mem_free( m_fontcache );
            m_fontcache=NULL ;
        }
    }
    else if( m_req.reqsize>(int)sizeof(struct BITMAP) ) { 	// BITMAP
        AnsOk();
        if(m_fontcache ) {
            mem_free( m_fontcache );
            m_fontcache=NULL ;
        }
        m_fontcache = (struct BITMAP *)mem_addref(m_recvbuf, m_req.reqsize);
        m_fontcache->bits = ((UINT8 *)m_fontcache)+sizeof(BITMAP) ;		// replace bits
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawText()
{
    if( m_fontcache && m_req.reqsize>2*(int)sizeof(int) ) {
        int * param = (int *)m_recvbuf ;
        char * text = (char *)&param[2] ;
        AnsOk();
        draw_text( param[0], param[1], text, m_fontcache );
    }
    else {
        AnsError();
    }
}

void dvrsvr::ReqDrawTextEx()
{
    if( m_fontcache && m_req.reqsize>4*(int)sizeof(int) ) {
        int * param = (int *)m_recvbuf ;
        char * text = (char *)&param[4] ;
        AnsOk();
        draw_text_ex( param[0], param[1], text, m_fontcache, param[2], param[3] );
    }
    else {
        AnsError();
    }
}
