// SPDX-License-Identifier: MIT
using System;
using System.Threading;
using NetBench360;
class ClientTest {
    static int Main(string[] args) {
        Thread.CurrentThread.CurrentCulture=System.Globalization.CultureInfo.InvariantCulture;
        try {
            using(var cancel=new CancellationTokenSource()) {
                if(args.Length>3) cancel.CancelAfter(int.Parse(args[3]));
                using(var engine=new Engine(cancel.Token,null)) {
                    int buffer=args.Length>4 ? int.Parse(args[4]) : 65536;
                    var r=engine.Run("127.0.0.1",int.Parse(args[0]),int.Parse(args[1]),int.Parse(args[2]),buffer,1);
                    if(args.Length>4 && (r.RequestedBuffer!=buffer || r.PcSendBuffer!=buffer || r.PcRecvBuffer!=buffer ||
                        r.XboxSendBuffer!=65536 || r.XboxRecvBuffer!=65536)) throw new Exception("Buffer metadata did not match actual settings.");
                    Console.WriteLine("PASS direction={0} bytes={1} seconds={2:F6} MiB/s={3:F2}",r.Direction,r.Bytes,r.Microseconds/1000000.0,r.Rate);
                }
            }
            return 0;
        } catch(OperationCanceledException) { Console.WriteLine("CANCELLED"); return 2; }
          catch(Exception e) { Console.WriteLine("ERROR "+e.Message); return 1; }
    }
}
