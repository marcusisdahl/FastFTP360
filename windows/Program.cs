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

namespace NetBench360 {
    sealed class MainForm : Form {
        readonly Color navy=Color.FromArgb(15,23,37), panel=Color.FromArgb(25,37,55), ink=Color.FromArgb(226,235,244), accent=Color.FromArgb(62,217,188);
        TextBox ip,driver; ComboBox direction,duration,repeats,buffers; Button start,stop,save;
        Label status,live,upload,download,detail; ProgressBar progress; DataGridView grid;
        readonly List<TestResult> results=new List<TestResult>();
        CancellationTokenSource cancellation; bool running,closeRequested; int testSeconds;
        public MainForm() {
            Text="NetBench360 - RAM network benchmark"; ClientSize=new Size(1050,735); MinimumSize=new Size(1066,774);
            StartPosition=FormStartPosition.CenterScreen; BackColor=navy; ForeColor=ink; Font=new Font("Segoe UI",10);
            AutoScaleMode=AutoScaleMode.Dpi;
            LabelAt("NETBENCH360",26,20,520,40,24,FontStyle.Bold);
            LabelAt("Memory to memory. Measure the network without the Xbox drive.",28,65,920,28,11);
            LabelAt("XBOX IP ADDRESS",28,119,190,22,9); ip=TextAt("",28,144,190); ip.AccessibleName="Xbox IP address";
            LabelAt("DRIVER LABEL",236,119,210,22,9); driver=TextAt("TxScan",236,144,210); driver.AccessibleName="Driver label";
            LabelAt("DIRECTION",464,119,248,22,9); direction=Choice(new[]{"Both directions","PC to Xbox","Xbox to PC"},464,144,248,0);
            LabelAt("DURATION",730,119,128,22,9); duration=Choice(new[]{"10 seconds","20 seconds","30 seconds","60 seconds"},730,144,128,1);
            LabelAt("REPEATS",876,119,146,22,9); repeats=Choice(new[]{"1 per direction","2 per direction","3 per direction","5 per direction"},876,144,146,2);
            LabelAt("SOCKET BUFFERS",28,195,190,22,9); buffers=Choice(new[]{"16 KiB","32 KiB","64 KiB","128 KiB"},28,219,190,2);
            LabelAt("Keep settings identical when comparing drivers. The label does not change the Xbox plugin.",236,221,770,28,10);
            start=ButtonAt("Start benchmark",28,272,190,accent,navy); start.Click+=Start;
            stop=ButtonAt("Stop",236,272,100,panel,ink); stop.Enabled=false; stop.Click+=(s,e)=>{ if(cancellation!=null) cancellation.Cancel(); status.Text="Stopping..."; };
            save=ButtonAt("Save CSV",878,272,144,panel,ink); save.Enabled=false; save.Click+=Save;
            status=LabelAt("Launch the Xbox app, press A, then enter its IP address here.",28,332,994,30,11);
            live=LabelAt("READY",28,370,410,46,24,FontStyle.Bold);
            detail=LabelAt("Single TCP stream  /  1 MiB data check  /  32 MiB warm-up",450,380,572,28,10);
            progress=new ProgressBar { Left=28,Top=424,Width=994,Height=7,Style=ProgressBarStyle.Continuous }; Controls.Add(progress);
            upload=LabelAt("PC TO XBOX     -- MiB/s",28,452,480,34,15,FontStyle.Bold);
            download=LabelAt("XBOX TO PC     -- MiB/s",540,452,482,34,15,FontStyle.Bold);
            grid=new DataGridView { Left=28,Top=502,Width=994,Height=178,ReadOnly=true,AllowUserToAddRows=false,
                AllowUserToDeleteRows=false,AllowUserToResizeRows=false,RowHeadersVisible=false,BackgroundColor=panel,
                BorderStyle=BorderStyle.None,AutoSizeColumnsMode=DataGridViewAutoSizeColumnsMode.Fill,
                SelectionMode=DataGridViewSelectionMode.FullRowSelect,EnableHeadersVisualStyles=false,Anchor=AnchorStyles.Left|AnchorStyles.Right|AnchorStyles.Top|AnchorStyles.Bottom };
            grid.ColumnHeadersDefaultCellStyle=new DataGridViewCellStyle { BackColor=panel,ForeColor=ink,Font=new Font("Segoe UI",9,FontStyle.Bold) };
            grid.DefaultCellStyle=new DataGridViewCellStyle { BackColor=navy,ForeColor=ink,SelectionBackColor=Color.FromArgb(42,73,85),SelectionForeColor=Color.White,Font=new Font("Segoe UI",9) };
            foreach(string name in new[]{"Run","Driver","Direction","Average MiB/s","Low (1s)","High (1s)","Data check"}) grid.Columns.Add(name,name);
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
            status.Text=(p.Direction==1 ? "PC to Xbox" : "Xbox to PC")+"  /  "+p.Phase;
            live.Text=p.Phase=="Measuring" || p.Phase=="Complete" ? p.Rate.ToString("F2")+" MiB/s" : p.Phase.ToUpperInvariant();
            detail.Text=String.Format("{0:F1} MiB transferred  /  {1:F1} seconds",p.Bytes/1048576.0,p.Seconds);
            progress.Value=p.Phase=="Complete" ? 100 : Math.Max(0,Math.Min(100,(int)(p.Seconds/testSeconds*100)));
        }); }
        void SetRunning(bool value) {
            running=value; start.Enabled=!value; stop.Enabled=value; save.Enabled=!value && results.Count>0;
            foreach(Control c in new Control[]{ip,driver,direction,duration,repeats,buffers}) c.Enabled=!value;
        }
        async void Start(object sender,EventArgs args) {
            IPAddress parsed;
            if(!IPAddress.TryParse(ip.Text.Trim(),out parsed) || parsed.AddressFamily!=System.Net.Sockets.AddressFamily.InterNetwork) {
                status.Text="Enter the IPv4 address shown on the Xbox, for example 192.168.1.50."; ip.Focus(); return;
            }
            string address=parsed.ToString(),label=driver.Text.Trim(); int selected=direction.SelectedIndex;
            int count=new[]{1,2,3,5}[repeats.SelectedIndex],size=new[]{16384,32768,65536,131072}[buffers.SelectedIndex];
            testSeconds=new[]{10,20,30,60}[duration.SelectedIndex]; cancellation=new CancellationTokenSource(); SetRunning(true);
            try {
                await Task.Run(()=>{
                    for(int run=1;run<=count;run++) foreach(int mode in selected==0 ? new[]{1,2} : new[]{selected}) {
                        cancellation.Token.ThrowIfCancellationRequested();
                        using(var engine=new Engine(cancellation.Token,UpdateProgress)) {
                            var result=engine.Run(address,36020,mode,testSeconds,size,32); result.Driver=label; result.Run=run;
                            Ui(()=>AddResult(result));
                        }
                    }
                });
                status.Text="All requested tests completed. Save CSV to keep these results.";
            } catch(OperationCanceledException) { status.Text="Stopped. Completed results are retained; the interrupted run was discarded."; live.Text="STOPPED"; }
              catch(Exception e) { status.Text="Test stopped: "+e.Message; live.Text="CHECK CONNECTION"; }
            finally { SetRunning(false); cancellation.Dispose(); cancellation=null; if(closeRequested) Close(); }
        }
        void AddResult(TestResult r) {
            results.Add(r); grid.Rows.Add(r.Run,r.Driver,r.DirectionText,r.Rate.ToString("F2"),r.Minimum.ToString("F2"),r.Maximum.ToString("F2"),"1 MiB passed");
            if(grid.Rows.Count>0) grid.FirstDisplayedScrollingRowIndex=grid.Rows.Count-1;
            // Summarize only the same labeled configuration, never blend driver builds.
            foreach(int mode in new[]{1,2}) {
                double[] rates=results.Where(x=>x.Direction==mode && x.Address==r.Address && x.Driver==r.Driver && x.RequestedBuffer==r.RequestedBuffer && x.RequestedSeconds==r.RequestedSeconds).Select(x=>x.Rate).OrderBy(x=>x).ToArray();
                double median=rates.Length==0 ? 0 : (rates[(rates.Length-1)/2]+rates[rates.Length/2])/2;
                (mode==1 ? upload : download).Text=(mode==1 ? "PC TO XBOX     " : "XBOX TO PC     ")+(rates.Length==0 ? "--" : median.ToString("F2"))+" MiB/s median";
            }
        }
        static string Csv(object value) { return "\""+Convert.ToString(value,CultureInfo.InvariantCulture).Replace("\"","\"\"")+"\""; }
        void Save(object sender,EventArgs args) {
            using(var dialog=new SaveFileDialog { Filter="CSV results (*.csv)|*.csv",FileName="NetBench360-"+DateTime.Now.ToString("yyyyMMdd-HHmm")+".csv" }) {
                if(dialog.ShowDialog()!=DialogResult.OK) return;
                var lines=new List<string> { "utc,driver_label,xbox_ip,run,direction,requested_seconds,receiver_bytes,receiver_seconds,average_MiB_s,min_1s_MiB_s,max_1s_MiB_s,full_sample_count,integrity_bytes,requested_socket_bytes,pc_send_bytes,pc_receive_bytes,xbox_send_bytes,xbox_receive_bytes,warmup_MiB" };
                foreach(var r in results) lines.Add(String.Join(",",new object[]{r.Utc,r.Driver,r.Address,r.Run,r.DirectionText,r.RequestedSeconds,r.Bytes,r.Microseconds/1000000.0,r.Rate,r.Minimum,r.Maximum,r.Samples,1048576,r.RequestedBuffer,r.PcSendBuffer,r.PcRecvBuffer,r.XboxSendBuffer,r.XboxRecvBuffer,32}.Select(Csv)));
                try { File.WriteAllLines(dialog.FileName,lines,new UTF8Encoding(true)); status.Text="Saved "+results.Count+" completed results."; }
                catch(Exception e) { status.Text="Could not save: "+e.Message; }
            }
        }
        [STAThread] static void Main() { Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false); Application.Run(new MainForm()); }
    }
}
