// SPDX-License-Identifier: MIT
using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;

namespace NetBench360 {
    public sealed class TestResult {
        public string Driver, Address, Utc;
        public int Direction, Run, RequestedSeconds, RequestedBuffer, PcSendBuffer, PcRecvBuffer, XboxSendBuffer, XboxRecvBuffer, Streams=1;
        public int XboxSendLimit, RequestedXboxSendLimit; public bool XboxDrawingPaused;
        public ulong Bytes, Microseconds, Verified;
        public bool VerifyAll;
        public uint Samples;
        public double Minimum, Maximum;
        public double Rate { get { return Microseconds == 0 ? 0 : Bytes / 1.048576 / Microseconds; } }
        public string DirectionText { get { return Direction == 1 ? "PC to Xbox" : "Xbox to PC"; } }
    }
    public sealed class Update {
        public string Phase; public int Direction; public ulong Bytes; public double Seconds, Rate;
    }
    public sealed class Engine : IDisposable {
        const uint Magic=0x4e423336, Version=3;
        const int Chunk=65536, Integrity=1048576, ResultSize=72;
        public const uint FlagVerifyAll=0x100;
        readonly Socket socket;
        readonly CancellationToken cancel;
        readonly Action<Update> update;
        readonly Stopwatch session=Stopwatch.StartNew();
        double lastActivity;
        string phase="Connecting";
        public Engine(CancellationToken token, Action<Update> progress) {
            cancel=token; update=progress;
            socket=new Socket(AddressFamily.InterNetwork,SocketType.Stream,ProtocolType.Tcp);
        }
        public void Dispose() { socket.Dispose(); }
        public static uint Read32(byte[] data,int at) {
            return ((uint)data[at]<<24)|((uint)data[at+1]<<16)|((uint)data[at+2]<<8)|data[at+3];
        }
        public static ulong Read64(byte[] data,int at) { return ((ulong)Read32(data,at)<<32)|Read32(data,at+4); }
        public static void Write32(byte[] data,int at,uint v) {
            data[at]=(byte)(v>>24); data[at+1]=(byte)(v>>16); data[at+2]=(byte)(v>>8); data[at+3]=(byte)v;
        }
        public static void Write64(byte[] data,int at,ulong v) { Write32(data,at,(uint)(v>>32)); Write32(data,at+4,(uint)v); }
        public static byte[] Pattern(uint seed) {
            byte[] data=new byte[Chunk]; uint state=seed==0 ? 1 : seed;
            for(int i=0;i<data.Length;i++) { state^=state<<13; state^=state>>17; state^=state<<5; data[i]=(byte)state; }
            return data;
        }
        void Notify(string phase,int direction,ulong bytes,double seconds,double rate) {
            this.phase=phase;
            if(update!=null) update(new Update { Phase=phase,Direction=direction,Bytes=bytes,Seconds=seconds,Rate=rate });
        }
        void Wait(SelectMode mode) {
            for(;;) {
                cancel.ThrowIfCancellationRequested();
                double now=session.Elapsed.TotalSeconds;
                if(now>180) throw new TimeoutException("The session exceeded its time limit.");
                if(now-lastActivity>15) throw new TimeoutException("No data for 15 seconds. Check the Xbox screen and connection.");
                if(socket.Poll(100000,mode)) return;
            }
        }
        void SendAll(byte[] buffer,int length) {
            int at=0;
            while(at<length) {
                Wait(SelectMode.SelectWrite);
                try { int count=socket.Send(buffer,at,Math.Min(16384,length-at),SocketFlags.None);
                    if(count==0) throw new InvalidOperationException("Connection closed while sending.");
                    at+=count; lastActivity=session.Elapsed.TotalSeconds;
                } catch(SocketException e) { if(e.SocketErrorCode!=SocketError.WouldBlock) throw; Thread.Sleep(1); }
            }
        }
        int Receive(byte[] buffer,int at,int length) {
            for(;;) {
                Wait(SelectMode.SelectRead);
                try { int count=socket.Receive(buffer,at,length,SocketFlags.None);
                    if(count>0) lastActivity=session.Elapsed.TotalSeconds;
                    return count;
                } catch(SocketException e) { if(e.SocketErrorCode!=SocketError.WouldBlock) throw; Thread.Sleep(1); }
            }
        }
        void ReceiveAll(byte[] buffer) {
            int at=0;
            while(at<buffer.Length) { int count=Receive(buffer,at,buffer.Length-at);
                if(count==0) {
                    string hint=phase=="Negotiating" ? " Check the error line on the Xbox screen and use the updated Xbox XEX." : "";
                    throw new InvalidOperationException("Xbox closed the connection during "+phase.ToLowerInvariant()+" ("+at+"/"+buffer.Length+" bytes)."+hint);
                }
                at+=count; }
        }
        // Compares stream bytes [verified, limit) with the repeating pattern. Stream
        // offset 'at' is buffer[0]; the 32 bytes before it are kept in 'tail'.
        static bool VerifyStream(byte[] expected,byte[] tail,ulong at,byte[] buffer,ref ulong verified,ulong limit) {
            while(verified<limit) {
                byte[] source; int index; ulong available;
                if(verified<at) { source=tail; index=32-(int)(at-verified); available=at-verified; }
                else { source=buffer; index=(int)(verified-at); available=limit-verified; }
                if(verified+available>limit) available=limit-verified;
                int offset=(int)(verified%Chunk); int count=(int)Math.Min((ulong)(Chunk-offset),available);
                for(int i=0;i<count;i++) if(source[index+i]!=expected[offset+i]) return false;
                verified+=(ulong)count;
            }
            return true;
        }
        void FixedTransfer(bool sender,byte[] expected,byte[] buffer,int bytes,bool verify,Action ready=null) {
            for(int at=0;at<bytes;at+=Chunk) {
                if(sender) SendAll(expected,Chunk);
                else {
                    ReceiveAll(buffer);
                    if(verify) for(int i=0;i<Chunk;i++) if(buffer[i]!=expected[i])
                        throw new InvalidOperationException("Data integrity check FAILED. Stop testing this driver build.");
                }
            }
            byte[] ack=new byte[4];
            // 'ready' lets parallel streams start their timed phase together: the
            // sender waits after the receiver's ACK, the receiver before sending it.
            if(sender) { ReceiveAll(ack); if(Read32(ack,0)!=0) throw new InvalidOperationException("The receiver rejected the data check."); if(ready!=null) ready(); }
            else { if(ready!=null) ready(); SendAll(ack,4); }
        }
        // bufferSize 0 keeps Windows' automatic socket buffers (receive window
        // autotuning stays on). verifyAll checks every timed byte at the receiver.
        // xboxSendLimit applies to each stream, so parallel streams add up.
        public TestResult Run(string address,int port,int direction,int seconds,int bufferSize,int warmupMiB,int xboxSendLimit=16,bool pauseXboxDrawing=true,bool verifyAll=false,Action beforeMeasure=null) {
            if(direction<1 || direction>2 || seconds<1 || seconds>60 || warmupMiB<1 || warmupMiB>64 ||
                (bufferSize!=0 && bufferSize!=16384 && bufferSize!=32768 && bufferSize!=65536 && bufferSize!=131072) ||
                (xboxSendLimit!=0 && xboxSendLimit!=8 && xboxSendLimit!=12 && xboxSendLimit!=16 && xboxSendLimit!=20 && xboxSendLimit!=24 && xboxSendLimit!=32))
                throw new ArgumentException("Unsupported benchmark settings.");
            Notify("Connecting",direction,0,0,0);
            IPAddress ip=IPAddress.Parse(address);
            if(ip.AddressFamily!=AddressFamily.InterNetwork) throw new ArgumentException("Use an IPv4 address.");
            // Apply buffer preferences before connection establishment.
            if(bufferSize!=0) { socket.SendBufferSize=bufferSize; socket.ReceiveBufferSize=bufferSize; }
            IAsyncResult connecting=socket.BeginConnect(new IPEndPoint(ip,port),null,null);
            using(WaitHandle wait=connecting.AsyncWaitHandle) {
                while(!wait.WaitOne(100)) {
                    cancel.ThrowIfCancellationRequested();
                    if(session.Elapsed.TotalSeconds>10) throw new TimeoutException("Cannot connect. Press A on the Xbox and check its IP address.");
                }
                socket.EndConnect(connecting);
            }
            socket.Blocking=false;
            lastActivity=session.Elapsed.TotalSeconds;
            uint seed=unchecked((uint)Environment.TickCount) ^ 0x360179u;
            byte[] request=new byte[40];
            uint options=pauseXboxDrawing ? 1u : 0u;
            int activeLimit=direction==2 ? xboxSendLimit : 0;
            uint[] fields={Magic,Version,(uint)direction|(verifyAll ? FlagVerifyAll : 0u),(uint)seconds,Chunk,(uint)bufferSize,(uint)warmupMiB,seed,(uint)xboxSendLimit,options};
            for(int i=0;i<fields.Length;i++) Write32(request,4*i,fields[i]);
            Notify("Negotiating",direction,0,0,0);
            SendAll(request,40); byte[] response=new byte[32]; ReceiveAll(response);
            if(Read32(response,0)!=Magic || Read32(response,4)!=Version || Read32(response,8)!=0 ||
                Read32(response,12)!=Chunk || Read32(response,24)!=Integrity || Read32(response,28)!=warmupMiB)
                throw new InvalidOperationException("The Xbox rejected the request, or is running a different protocol version. Update BOTH apps to v0.2.0.");
            byte[] settings=new byte[8]; ReceiveAll(settings);
            if(Read32(settings,0)!=activeLimit || Read32(settings,4)!=options)
                throw new InvalidOperationException("Xbox diagnostic settings did not match the request.");
            TestResult result=new TestResult { Direction=direction,RequestedSeconds=seconds,RequestedBuffer=bufferSize,
                Address=address,Utc=DateTime.UtcNow.ToString("o"),VerifyAll=verifyAll,PcSendBuffer=socket.SendBufferSize,PcRecvBuffer=socket.ReceiveBufferSize,
                XboxSendBuffer=(int)Read32(response,16),XboxRecvBuffer=(int)Read32(response,20),
                XboxSendLimit=activeLimit,RequestedXboxSendLimit=xboxSendLimit,XboxDrawingPaused=pauseXboxDrawing };
            bool sender=direction==1;
            byte[] expected=Pattern(seed),buffer=new byte[Chunk];
            Notify("Checking data",direction,0,0,0); FixedTransfer(sender,expected,buffer,Integrity,true);
            Notify("Warming up",direction,0,0,0); FixedTransfer(sender,expected,buffer,warmupMiB*1048576,false,beforeMeasure);
            Notify("Measuring",direction,0,0,0);
            Stopwatch timed=Stopwatch.StartNew(); double lastSample=0,lastUpdate=0,currentRate=0; ulong sampleBytes=0;
            byte[] finish=new byte[32];
            for(;;) {
                if(sender) {
                    if(timed.Elapsed.TotalSeconds>=seconds) break;
                    SendAll(expected,Chunk); result.Bytes+=Chunk;
                } else {
                    int got=Receive(buffer,0,Chunk); if(got==0) break;
                    if(verifyAll) {
                        ulong total=result.Bytes+(ulong)got;
                        if(!VerifyStream(expected,finish,result.Bytes,buffer,ref result.Verified,total>32 ? total-32 : 0))
                            throw new InvalidOperationException("Data integrity check FAILED during the timed transfer. Stop testing this driver build.");
                    }
                    if(got>=32) Buffer.BlockCopy(buffer,got-32,finish,0,32);
                    else { Buffer.BlockCopy(finish,got,finish,0,32-got); Buffer.BlockCopy(buffer,0,finish,32-got,got); }
                    result.Bytes+=(uint)got;
                    if(timed.Elapsed.TotalSeconds>seconds+15) throw new TimeoutException("The sender exceeded the test duration.");
                }
                double current=timed.Elapsed.TotalSeconds;
                if(current-lastSample>=1) {
                    currentRate=(result.Bytes-sampleBytes)/(current-lastSample)/1048576;
                    if(result.Samples==0 || currentRate<result.Minimum) result.Minimum=currentRate;
                    if(currentRate>result.Maximum) result.Maximum=currentRate;
                    ++result.Samples; sampleBytes=result.Bytes; lastSample=current;
                }
                if(current-lastUpdate>=0.25) { Notify("Measuring",direction,result.Bytes,current,currentRate); lastUpdate=current; }
            }
            result.Microseconds=(ulong)(timed.Elapsed.TotalSeconds*1000000);
            if(result.Bytes==0 || result.Microseconds==0) throw new InvalidOperationException("Empty measurement.");
            byte[] wire=new byte[ResultSize];
            if(sender) {
                Write32(finish,0,Magic); Write32(finish,4,Version); Write32(finish,8,(uint)direction);
                Write64(finish,16,result.Bytes); Write64(finish,24,result.Microseconds); SendAll(finish,32);
                socket.Shutdown(SocketShutdown.Send); ReceiveAll(wire);
                ulong elapsed=Read64(wire,24);
                if(Read32(wire,0)!=Magic || Read32(wire,4)!=Version || Read32(wire,8)!=direction || Read32(wire,12)!=0 ||
                    Read64(wire,16)!=result.Bytes || elapsed==0 || elapsed>180000000 || Read32(wire,52)!=Integrity ||
                    Read32(wire,56)!=activeLimit || Read32(wire,60)!=options)
                    throw new InvalidOperationException("Receiver totals did not match the transfer.");
                result.Microseconds=elapsed; result.Minimum=Read32(wire,32)/1024.0; result.Maximum=Read32(wire,36)/1024.0;
                result.Samples=Read32(wire,48); result.Verified=Read64(wire,64);
                if(verifyAll && result.Verified!=result.Bytes) throw new InvalidOperationException("The Xbox did not check all data. Use the updated Xbox XEX.");
            } else {
                ulong sentTime=Read64(finish,24);
                if(result.Bytes<=32 || Read32(finish,0)!=Magic || Read32(finish,4)!=Version || Read32(finish,8)!=direction ||
                    Read32(finish,12)!=0 || Read64(finish,16)!=result.Bytes-32 || (result.Bytes-32)%Chunk!=0 ||
                    sentTime<(ulong)seconds*1000000 || sentTime>(ulong)(seconds+15)*1000000)
                    throw new InvalidOperationException("Transfer ended without a valid finish record.");
                result.Bytes-=32;
                if(verifyAll && result.Verified!=result.Bytes) throw new InvalidOperationException("The data check did not cover the whole transfer.");
                if(result.Samples==0) result.Minimum=result.Maximum=result.Rate;
                Write32(wire,0,Magic); Write32(wire,4,Version); Write32(wire,8,(uint)direction);
                Write64(wire,16,result.Bytes); Write64(wire,24,result.Microseconds);
                Write32(wire,32,(uint)(result.Minimum*1024)); Write32(wire,36,(uint)(result.Maximum*1024));
                Write32(wire,40,(uint)result.XboxSendBuffer); Write32(wire,44,(uint)result.XboxRecvBuffer);
                Write32(wire,48,result.Samples); Write32(wire,52,Integrity);
                Write32(wire,56,(uint)activeLimit); Write32(wire,60,options); Write64(wire,64,result.Verified); SendAll(wire,ResultSize);
            }
            Notify("Complete",direction,result.Bytes,result.Microseconds/1000000.0,result.Rate);
            return result;
        }
    }
}
