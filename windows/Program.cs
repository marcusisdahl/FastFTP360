// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace FastFTP {
    sealed class MainForm : Form {
        readonly Color navy=Color.FromArgb(15,23,37), panel=Color.FromArgb(25,37,55), ink=Color.FromArgb(226,235,244), accent=Color.FromArgb(62,217,188);
        TextBox ip,driver; ComboBox direction,duration,repeats,buffers,limits,streams,check; CheckBox pauseDrawing; Button start,stop,save;
        Label status,live,upload,download,detail; ProgressBar progress; DataGridView grid;
        readonly List<TestResult> results=new List<TestResult>();
        CancellationTokenSource cancellation; bool running,closeRequested; int testSeconds,activeLimit;
        public MainForm() {
            Text="FastFTP network test v0.5.0"; ClientSize=new Size(1050,735); MinimumSize=new Size(1066,774);
            StartPosition=FormStartPosition.CenterScreen; BackColor=navy; ForeColor=ink; Font=new Font("Segoe UI",10);
            AutoScaleMode=AutoScaleMode.Dpi;
            LabelAt("FASTFTP NETWORK TEST",26,20,520,40,24,FontStyle.Bold);
            LabelAt("Diagnostic defaults reduce Xbox sending load. Capped results do not measure maximum speed.",28,65,994,28,11);
            LabelAt("XBOX IP ADDRESS",28,119,190,22,9); ip=TextAt("",28,144,190); ip.AccessibleName="Xbox IP address";
            LabelAt("DRIVER LABEL",236,119,210,22,9); driver=TextAt("TxScan",236,144,210); driver.AccessibleName="Driver label";
            LabelAt("DIRECTION",464,119,248,22,9); direction=Choice(new[]{"Both directions","PC to Xbox","Xbox to PC"},464,144,248,2);
            LabelAt("DURATION",730,119,128,22,9); duration=Choice(new[]{"10 seconds","20 seconds","30 seconds","60 seconds"},730,144,128,0);
            LabelAt("REPEATS",876,119,146,22,9); repeats=Choice(new[]{"1 per direction","2 per direction","3 per direction","5 per direction"},876,144,146,0);
            LabelAt("PC SOCKET BUFFERS",28,195,190,22,9); buffers=Choice(new[]{"16 KiB","32 KiB","64 KiB","128 KiB","Windows automatic"},28,219,190,2);
            LabelAt("XBOX SEND LIMIT",236,195,210,22,9); limits=Choice(new[]{"8 MiB/s","12 MiB/s","16 MiB/s","20 MiB/s","24 MiB/s","32 MiB/s","Unlimited"},236,219,210,2);
            pauseDrawing=new CheckBox { Text="Pause Xbox drawing during test",Left=464,Top=219,Width=290,Height=28,Checked=true,ForeColor=ink,BackColor=navy }; Controls.Add(pauseDrawing);
            LabelAt("Xbox buffers and CPU: Settings tab. Actual sizes in CSV.",764,214,258,44,9);
            LabelAt("TCP STREAMS",354,256,150,18,8); streams=Choice(new[]{"1 stream","2 streams","4 streams"},354,276,150,0);
            LabelAt("DATA CHECK",522,256,170,18,8); check=Choice(new[]{"First 1 MiB","All data"},522,276,170,0);
            LabelAt("The send limit applies to each stream.",710,276,160,40,8);
            start=ButtonAt("Start benchmark",28,272,190,accent,navy); start.Click+=Start;
            stop=ButtonAt("Stop",236,272,100,panel,ink); stop.Enabled=false; stop.Click+=(s,e)=>{ if(cancellation!=null) cancellation.Cancel(); status.Text="Stopping..."; };
            save=ButtonAt("Save CSV",878,272,144,panel,ink); save.Enabled=false; save.Click+=Save;
            status=LabelAt("Launch the Xbox app, press A, then enter its IP address here.",28,332,994,30,11);
            live=LabelAt("READY",28,370,410,46,24,FontStyle.Bold);
            detail=LabelAt("1 to 4 TCP streams  /  data check  /  32 MiB warm-up",450,380,572,28,10);
            progress=new ProgressBar { Left=28,Top=424,Width=994,Height=7,Style=ProgressBarStyle.Continuous }; Controls.Add(progress);
            upload=LabelAt("PC TO XBOX     -- MiB/s",28,452,480,34,15,FontStyle.Bold);
            download=LabelAt("XBOX TO PC     -- MiB/s",540,452,482,34,15,FontStyle.Bold);
            grid=new DataGridView { Left=28,Top=502,Width=994,Height=178,ReadOnly=true,AllowUserToAddRows=false,
                AllowUserToDeleteRows=false,AllowUserToResizeRows=false,RowHeadersVisible=false,BackgroundColor=panel,
                BorderStyle=BorderStyle.None,AutoSizeColumnsMode=DataGridViewAutoSizeColumnsMode.Fill,
                SelectionMode=DataGridViewSelectionMode.FullRowSelect,EnableHeadersVisualStyles=false,Anchor=AnchorStyles.Left|AnchorStyles.Right|AnchorStyles.Top|AnchorStyles.Bottom };
            grid.ColumnHeadersDefaultCellStyle=new DataGridViewCellStyle { BackColor=panel,ForeColor=ink,Font=new Font("Segoe UI",9,FontStyle.Bold) };
            grid.DefaultCellStyle=new DataGridViewCellStyle { BackColor=navy,ForeColor=ink,SelectionBackColor=Color.FromArgb(42,73,85),SelectionForeColor=Color.White,Font=new Font("Segoe UI",9) };
            foreach(string name in new[]{"Run","Driver","Direction","Streams","Average MiB/s","Low (1s)","High (1s)","Data check"}) grid.Columns.Add(name,name);
            grid.Columns[0].FillWeight=40; grid.Columns[2].FillWeight=115; Controls.Add(grid);
            var footer=LabelAt("Rates are receiver-reported. Results stay in memory until you save. Use a trusted local network.",28,696,994,23,9);
            footer.Anchor=AnchorStyles.Bottom|AnchorStyles.Left;
            FormClosing+=(s,e)=>{ if(running) { closeRequested=true; cancellation.Cancel(); e.Cancel=true; status.Text="Stopping before closing..."; } };
        }
        Label LabelAt(string text,int x,int y,int w,int h,float size,FontStyle style=FontStyle.Regular) {
            var c=new Label { Text=text,Left=x,Top=y,Width=w,Height=h,ForeColor=ink,Font=new Font("Segoe UI",size,style),AutoEllipsis=true }; Controls.Add(c); return c;
        }
        TextBox TextAt(string text,int x,int y,int width) {
            var c=new TextBox { Text=text,Left=x,Top=y,Width=width,BackColor=panel,ForeColor=ink,BorderStyle=BorderStyle.FixedSingle,Font=new Font("Segoe UI",12) }; Controls.Add(c); return c;
        }
        ComboBox Choice(string[] values,int x,int y,int w,int selected) {
            var c=new ComboBox { Left=x,Top=y,Width=w,DropDownStyle=ComboBoxStyle.DropDownList,BackColor=panel,ForeColor=ink,FlatStyle=FlatStyle.Flat };
            c.Items.AddRange(values); c.SelectedIndex=selected; Controls.Add(c); return c;
        }
        Button ButtonAt(string text,int x,int y,int w,Color back,Color fore) {
            var c=new Button { Text=text,Left=x,Top=y,Width=w,Height=40,BackColor=back,ForeColor=fore,FlatStyle=FlatStyle.Flat }; c.FlatAppearance.BorderSize=0; Controls.Add(c); return c;
        }
        void Ui(Action action) { if(!IsDisposed && IsHandleCreated) BeginInvoke(action); }
        void UpdateProgress(Update p) { Ui(()=>{
            status.Text=(p.Direction==1 ? "PC to Xbox" : "Xbox to PC")+"  /  "+p.Phase+
                (p.Direction==2 && activeLimit>0 ? "  /  CAPPED AT "+activeLimit+" MiB/s" : "");
            live.Text=p.Phase=="Measuring" || p.Phase=="Complete" ? p.Rate.ToString("F2")+" MiB/s" : p.Phase.ToUpperInvariant();
            detail.Text=String.Format("{0:F1} MiB transferred  /  {1:F1} seconds",p.Bytes/1048576.0,p.Seconds);
            progress.Value=p.Phase=="Complete" ? 100 : Math.Max(0,Math.Min(100,(int)(p.Seconds/testSeconds*100)));
        }); }
        void SetRunning(bool value) {
            running=value; start.Enabled=!value; stop.Enabled=value; save.Enabled=!value && results.Count>0;
            foreach(Control c in new Control[]{ip,driver,direction,duration,repeats,buffers,limits,pauseDrawing,streams,check}) c.Enabled=!value;
        }
        async void Start(object sender,EventArgs args) {
            IPAddress parsed;
            if(!IPAddress.TryParse(ip.Text.Trim(),out parsed) || parsed.AddressFamily!=System.Net.Sockets.AddressFamily.InterNetwork) {
                status.Text="Enter the IPv4 address shown on the Xbox, for example 192.168.1.50."; ip.Focus(); return;
            }
            string address=parsed.ToString(),label=driver.Text.Trim(); int selected=direction.SelectedIndex;
            int count=new[]{1,2,3,5}[repeats.SelectedIndex],size=new[]{16384,32768,65536,131072,0}[buffers.SelectedIndex];
            int streamCount=new[]{1,2,4}[streams.SelectedIndex]; bool verifyAll=check.SelectedIndex==1;
            int limit=new[]{8,12,16,20,24,32,0}[limits.SelectedIndex]; bool paused=pauseDrawing.Checked;
            activeLimit=limit*streamCount;
            testSeconds=new[]{10,20,30,60}[duration.SelectedIndex]; cancellation=new CancellationTokenSource(); SetRunning(true);
            try {
                await Task.Run(()=>{
                    for(int run=1;run<=count;run++) foreach(int mode in selected==0 ? new[]{1,2} : new[]{selected}) {
                        cancellation.Token.ThrowIfCancellationRequested();
                        var result=RunBatch(address,mode,testSeconds,size,limit,paused,verifyAll,streamCount,cancellation.Token);
                        result.Driver=label; result.Run=run;
                        Ui(()=>AddResult(result));
                    }
                });
                status.Text="All requested tests completed. Save CSV to keep these results.";
            } catch(OperationCanceledException) { status.Text="Stopped. Completed results are retained; the interrupted run was discarded."; live.Text="STOPPED"; }
              catch(Exception e) { status.Text="Test stopped: "+e.Message; live.Text="CHECK CONNECTION"; }
            finally { SetRunning(false); cancellation.Dispose(); cancellation=null; if(closeRequested) Close(); }
        }
        // Runs one test over several parallel TCP connections. Every stream is a
        // normal protocol session; their timed phases start together and the
        // combined result is total payload over the longest receiver time.
        TestResult RunBatch(string address,int mode,int seconds,int size,int limit,bool paused,bool verifyAll,int streamCount,CancellationToken token) {
            if(streamCount==1) using(var engine=new Engine(token,UpdateProgress)) return engine.Run(address,36020,mode,seconds,size,32,limit,paused,verifyAll);
            var latest=new Update[streamCount];
            using(var linked=CancellationTokenSource.CreateLinkedTokenSource(token)) using(var barrier=new Barrier(streamCount)) {
                var tasks=new Task<TestResult>[streamCount];
                for(int i=0;i<streamCount;i++) {
                    int index=i;
                    Action<Update> progress=u=>{
                        Update total;
                        lock(latest) {
                            latest[index]=u; total=new Update { Phase=u.Phase,Direction=u.Direction };
                            foreach(var x in latest) if(x!=null) { total.Bytes+=x.Bytes; total.Rate+=x.Rate; total.Seconds=Math.Max(total.Seconds,x.Seconds); }
                        }
                        UpdateProgress(total);
                    };
                    tasks[i]=Task.Factory.StartNew(()=>{
                        try { using(var engine=new Engine(linked.Token,progress))
                            return engine.Run(address,36020,mode,seconds,size,32,limit,paused,verifyAll,()=>barrier.SignalAndWait(linked.Token)); }
                        catch { linked.Cancel(); throw; }
                    },TaskCreationOptions.LongRunning);
                }
                try { Task.WaitAll(tasks); }
                catch(AggregateException e) {
                    token.ThrowIfCancellationRequested();
                    Exception first=e.Flatten().InnerExceptions.FirstOrDefault(x=>!(x is OperationCanceledException));
                    if(first!=null) throw first;
                    throw new OperationCanceledException();
                }
                var parts=tasks.Select(t=>t.Result).ToArray(); var r=parts[0];
                var combined=new TestResult { Direction=mode,RequestedSeconds=seconds,RequestedBuffer=size,Address=address,Utc=r.Utc,
                    PcSendBuffer=r.PcSendBuffer,PcRecvBuffer=r.PcRecvBuffer,XboxSendBuffer=r.XboxSendBuffer,XboxRecvBuffer=r.XboxRecvBuffer,
                    Streams=streamCount,VerifyAll=verifyAll,RequestedXboxSendLimit=limit,XboxDrawingPaused=paused };
                foreach(var p in parts) {
                    combined.XboxSendLimit+=p.XboxSendLimit;
                    combined.Bytes+=p.Bytes; combined.Verified+=p.Verified; combined.Microseconds=Math.Max(combined.Microseconds,p.Microseconds);
                    combined.Minimum+=p.Minimum; combined.Maximum+=p.Maximum;
                    combined.Samples=combined.Samples==0 ? p.Samples : Math.Min(combined.Samples,p.Samples);
                }
                return combined;
            }
        }
        void AddResult(TestResult r) {
            results.Add(r); grid.Rows.Add(r.Run,r.Driver,r.DirectionText+(r.XboxSendLimit>0 ? " CAP "+r.XboxSendLimit : ""),r.Streams,r.Rate.ToString("F2"),r.Minimum.ToString("F2"),r.Maximum.ToString("F2"),r.VerifyAll ? "All data passed" : "1 MiB passed");
            if(grid.Rows.Count>0) grid.FirstDisplayedScrollingRowIndex=grid.Rows.Count-1;
            // Summarize only the same labeled configuration, never blend driver builds.
            foreach(int mode in new[]{1,2}) {
                double[] rates=results.Where(x=>x.Direction==mode && x.Address==r.Address && x.Driver==r.Driver && x.RequestedBuffer==r.RequestedBuffer && x.RequestedSeconds==r.RequestedSeconds && x.Streams==r.Streams && x.XboxDrawingPaused==r.XboxDrawingPaused && (mode==1 || x.RequestedXboxSendLimit==r.RequestedXboxSendLimit)).Select(x=>x.Rate).OrderBy(x=>x).ToArray();
                double median=rates.Length==0 ? 0 : (rates[(rates.Length-1)/2]+rates[rates.Length/2])/2;
                (mode==1 ? upload : download).Text=(mode==1 ? "PC TO XBOX   " : "XBOX TO PC   ")+(rates.Length==0 ? "--" : median.ToString("F2"))+" MiB/s"+
                    (mode==2 && r.RequestedXboxSendLimit>0 ? " (cap "+r.RequestedXboxSendLimit*r.Streams+")" : " median");
            }
        }
        static string Csv(object value) { return "\""+Convert.ToString(value,CultureInfo.InvariantCulture).Replace("\"","\"\"")+"\""; }
        void Save(object sender,EventArgs args) {
            using(var dialog=new SaveFileDialog { Filter="CSV results (*.csv)|*.csv",FileName="FastFTP-network-"+DateTime.Now.ToString("yyyyMMdd-HHmm")+".csv" }) {
                if(dialog.ShowDialog()!=DialogResult.OK) return;
                var lines=new List<string> { "utc,driver_label,xbox_ip,run,direction,requested_seconds,receiver_bytes,receiver_seconds,average_MiB_s,min_1s_MiB_s,max_1s_MiB_s,full_sample_count,integrity_bytes,requested_socket_bytes,pc_send_bytes,pc_receive_bytes,xbox_send_bytes,xbox_receive_bytes,warmup_MiB,xbox_send_limit_MiB_s,xbox_drawing_paused,streams,verified_bytes" };
                foreach(var r in results) lines.Add(String.Join(",",new object[]{r.Utc,r.Driver,r.Address,r.Run,r.DirectionText,r.RequestedSeconds,r.Bytes,r.Microseconds/1000000.0,r.Rate,r.Minimum,r.Maximum,r.Samples,1048576,r.RequestedBuffer,r.PcSendBuffer,r.PcRecvBuffer,r.XboxSendBuffer,r.XboxRecvBuffer,32,r.XboxSendLimit,r.XboxDrawingPaused,r.Streams,r.Verified}.Select(Csv)));
                try { File.WriteAllLines(dialog.FileName,lines,new UTF8Encoding(true)); status.Text="Saved "+results.Count+" completed results."; }
                catch(Exception e) { status.Text="Could not save: "+e.Message; }
            }
        }
        [STAThread] static void Main() { Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false); Application.Run(new MainForm()); }
    }
}
