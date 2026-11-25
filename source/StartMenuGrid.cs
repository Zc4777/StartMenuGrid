using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyFileVersion("2.0.3.0")]

internal static class Program
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);

    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--session")
        {
            try { new StandaloneRuntime().RunSession(); }
            catch (Exception error) { Debug.WriteLine(error); }
            return;
        }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        using (var instance = new Mutex(false, @"Local\StartMenuGrid.StandalonePanel"))
        {
            if (!instance.WaitOne(0))
            {
                MessageBox.Show("控制面板已经打开。", "开始菜单网格", MessageBoxButtons.OK);
                return;
            }

            try
            {
                ushort processMachine;
                ushort nativeMachine;
                using (var process = Process.GetCurrentProcess())
                {
                    if (Environment.OSVersion.Version.Build < 22000 ||
                        !IsWow64Process2(process.Handle, out processMachine, out nativeMachine) ||
                        nativeMachine != 0x8664)
                    {
                        MessageBox.Show("此版本需要 Windows 11 x64。", "开始菜单网格", MessageBoxButtons.OK);
                        return;
                    }
                }

                Application.Run(new GridForm(new StandaloneRuntime()));
            }
            finally
            {
                instance.ReleaseMutex();
            }
        }
    }
}

internal sealed class GridForm : Form
{
    private static readonly Color Ink = Color.FromArgb(25, 38, 57);
    private static readonly Color Muted = Color.FromArgb(74, 89, 110);
    private static readonly Color Blue = Color.FromArgb(25, 78, 176);
    private static readonly Color Surface = Color.FromArgb(242, 246, 252);
    private readonly StandaloneRuntime runtime;
    private readonly TextBox columnsInput;
    private readonly TextBox rowsInput;
    private readonly Button apply;
    private readonly Button restore;
    private readonly Label status;
    private bool busy;

    internal GridForm(StandaloneRuntime runtime)
    {
        this.runtime = runtime;
        Text = "开始菜单网格";
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        AutoScaleDimensions = new SizeF(96, 96);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(496, 342);
        BackColor = Color.White;
        ForeColor = Ink;
        Font = new Font("Microsoft YaHei UI", 10F);

        var top = new Panel { Bounds = new Rectangle(0, 0, 496, 105), BackColor = Surface };
        top.Controls.Add(MakeLabel("开始菜单网格", 28, 20, 440, 39, 21F, Ink, FontStyle.Bold));
        top.Controls.Add(MakeLabel("调整原生开始菜单里固定应用的每页排列。", 30, 64, 440, 23, 10F, Muted));
        Controls.Add(top);

        Controls.Add(MakeLabel("横向列数", 30, 124, 194, 24, 10F, Ink, FontStyle.Bold));
        Controls.Add(MakeLabel("纵向行数", 255, 124, 194, 24, 10F, Ink, FontStyle.Bold));
        columnsInput = MakeInput("横向列数", 30, 153);
        rowsInput = MakeInput("纵向行数", 255, 153);
        Controls.Add(columnsInput);
        Controls.Add(rowsInput);
        Controls.Add(MakeLabel("例如 8 列 × 5 行，每页最多显示 40 个固定项。", 30, 193, 440, 23, 9F, Muted));

        apply = MakeButton("应用网格", 30, 233, 122, Blue, Color.White);
        restore = MakeButton("恢复默认", 164, 233, 122, Surface, Ink);
        apply.Enabled = restore.Enabled = false;
        apply.Click += ApplyClicked;
        restore.Click += async (sender, args) => await RestoreAsync();
        Controls.Add(apply);
        Controls.Add(restore);
        AcceptButton = apply;

        status = MakeLabel("正在读取设置…", 30, 292, 436, 28, 9F, Muted);
        status.AccessibleName = "状态";
        Controls.Add(status);
        Shown += async (sender, args) => await LoadAsync();
        FormClosing += (sender, args) => { if (busy) args.Cancel = true; };
    }

    private Label MakeLabel(string text, int x, int y, int w, int h, float size, Color color,
        FontStyle style = FontStyle.Regular)
    {
        return new Label
        {
            Text = text, Bounds = new Rectangle(x, y, w, h), ForeColor = color,
            Font = new Font(Font.FontFamily, size, style), TextAlign = ContentAlignment.MiddleLeft,
            UseMnemonic = false
        };
    }

    private TextBox MakeInput(string name, int x, int y)
    {
        return new TextBox
        {
            AccessibleName = name, Bounds = new Rectangle(x, y, 211, 31),
            Font = new Font("Segoe UI", 13F), MaxLength = 2
        };
    }

    private Button MakeButton(string caption, int x, int y, int w, Color background, Color foreground)
    {
        var button = new Button
        {
            Text = caption, Bounds = new Rectangle(x, y, w, 37),
            FlatStyle = FlatStyle.Flat, BackColor = background, ForeColor = foreground,
            UseVisualStyleBackColor = false, Cursor = Cursors.Hand
        };
        button.FlatAppearance.BorderSize = 0;
        return button;
    }

    private async Task LoadAsync()
    {
        busy = true;
        try
        {
            Size grid = await Task.Run(() => runtime.LoadGrid());
            bool isDefault = grid.IsEmpty;
            SetInputs(isDefault ? SuggestedGrid() : grid);
            status.Text = isDefault ? "填好行列后点击应用。当前使用系统默认。"
                : "已读取保存的行列设置。";
            apply.Enabled = restore.Enabled = true;
        }
        catch (Exception error)
        {
            status.Text = "无法读取网格设置。";
            ShowError(error);
        }
        finally
        {
            busy = false;
        }
    }

    private async void ApplyClicked(object sender, EventArgs args)
    {
        int columns;
        int rows;
        var limit = GridLimit();
        if (!TryRead(columnsInput, limit.Width, out columns) || !TryRead(rowsInput, limit.Height, out rows))
        {
            status.Text = string.Format("主屏幕支持 1–{0} 列、1–{1} 行。", limit.Width, limit.Height);
            return;
        }
        await RunActionAsync(() => runtime.Apply(new Size(columns, rows)));
    }

    private async Task RestoreAsync()
    {
        if (await RunActionAsync(() =>
        {
            runtime.Restore();
            return "已恢复系统默认布局。";
        }))
            SetInputs(SuggestedGrid());
    }

    private async Task<bool> RunActionAsync(Func<string> action)
    {
        busy = true;
        apply.Enabled = restore.Enabled = columnsInput.Enabled = rowsInput.Enabled = false;
        status.Text = "正在更新开始菜单设置…";
        try
        {
            status.Text = await Task.Run(action);
            return true;
        }
        catch (Exception error)
        {
            status.Text = "操作未完成。";
            ShowError(error);
            return false;
        }
        finally
        {
            busy = false;
            apply.Enabled = restore.Enabled = columnsInput.Enabled = rowsInput.Enabled = true;
        }
    }

    private static bool TryRead(TextBox input, int limit, out int value)
    {
        return int.TryParse(input.Text.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out value)
            && value >= 1 && value <= limit;
    }

    private Size GridLimit()
    {
        var area = Screen.PrimaryScreen.WorkingArea;
        using (var graphics = CreateGraphics())
            return new Size(Math.Max(1, Math.Min(24, (int)((area.Width * 96 / graphics.DpiX - 90) / 96))),
                Math.Max(1, Math.Min(16, (int)((area.Height * 96 / graphics.DpiY - 414) / 84))));
    }

    private Size SuggestedGrid()
    {
        var limit = GridLimit();
        return new Size(Math.Min(8, limit.Width), Math.Min(5, limit.Height));
    }

    private void SetInputs(Size grid)
    {
        columnsInput.Text = grid.Width.ToString(CultureInfo.InvariantCulture);
        rowsInput.Text = grid.Height.ToString(CultureInfo.InvariantCulture);
    }

    private void ShowError(Exception error)
    {
        MessageBox.Show(this, error.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }
}
