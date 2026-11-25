using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;

internal sealed class StandaloneRuntime
{
    private const string Version = "2.0.1.0";
    private const string ApplicationFile = "StartMenuGrid.exe";
    private const string LayoutFile = "StartMenuGrid.Layout.dll";
    private const string StatusProperty = "StartMenuGrid.Layout.Status.2";
    private const string SessionLock = @"Local\StartMenuGrid.AutomaticSession";
    private const string HostLocation = @"SystemApps\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\StartMenuExperienceHost.exe";
    private const string StartMenuLocation = @"SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\StartMenu.dll";
    private const string StartMenuHash = "00EBBBCE3040A0B605F9F61F6ABC1BE9F4AA5B0A53823807145F7ED619862DAA";
    private const int MissingEndpoint = unchecked((int)0x80070490);
    private static readonly Guid TapClass = new Guid("c418beb0-594e-434c-8c27-9d0ace8a35b5");
    private readonly string root;
    private readonly string installDirectory;
    private readonly string profile;
    private readonly string taskName;
    private readonly string user;
    private readonly string windows;
    private readonly int session;

    private delegate bool WindowCallback(IntPtr window, IntPtr state);

    [DllImport("Windows.UI.Xaml.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int InitializeXamlDiagnosticsEx(string endpoint, uint processId,
        string diagnostics, string extensionPath, Guid classId, string initializationData);

    [DllImport("user32.dll")]
    private static extern bool EnumThreadWindows(uint thread, WindowCallback callback, IntPtr state);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetProp(IntPtr window, string name);

    internal StandaloneRuntime()
    {
        root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "StartMenuGrid");
        installDirectory = Path.Combine(root, Version);
        windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        using (var identity = WindowsIdentity.GetCurrent())
        {
            string sid = identity.User.Value;
            user = identity.Name;
            taskName = "StartMenuGrid-" + sid;
            profile = Path.Combine(root, "Profiles", sid);
        }
        using (var process = Process.GetCurrentProcess())
            session = process.SessionId;
    }

    internal Size LoadGrid()
    {
        try
        {
            var fields = File.ReadAllText(Path.Combine(profile, "grid.txt")).Trim().Split(' ');
            int columns, rows;
            if (fields.Length == 2 &&
                int.TryParse(fields[0], NumberStyles.None, CultureInfo.InvariantCulture, out columns) &&
                int.TryParse(fields[1], NumberStyles.None, CultureInfo.InvariantCulture, out rows) &&
                columns > 0 && columns <= 24 && rows > 0 && rows <= 16)
                return new Size(columns, rows);
        }
        catch (FileNotFoundException) { }
        catch (DirectoryNotFoundException) { }
        return Size.Empty;
    }

    internal string Apply(Size grid)
    {
        CheckWindowsBuild();
        if (LegacyGridLoaded())
            throw new InvalidOperationException("旧版网格仍在运行。请在旧版面板点击「恢复默认」，再应用独立版。");

        InstallBinaries();
        var previous = LoadGrid();
        dynamic folder = OpenTaskService().GetFolder(@"\");
        dynamic previousTask = FindLogonTask(folder);
        dynamic previousDefinition = previousTask == null ? null : previousTask.Definition;
        string installedApp = Path.Combine(installDirectory, ApplicationFile);
        bool previousWorker = previousDefinition != null && previousDefinition.Actions.Count == 1 &&
            !string.Equals(installedApp, (string)previousDefinition.Actions.Item(1).Path, StringComparison.OrdinalIgnoreCase);
        bool restartHost = PreviousLayoutLoaded();
        bool taskUpdated = false;
        try
        {
            if (previousWorker || restartHost)
            {
                WriteGrid(Size.Empty);
                WaitForSessionExit();
                RestartPreviousLayout();
            }
            WriteGrid(grid);
            int attach = TryAttach();
            if (attach < 0 && attach != MissingEndpoint)
                throw new InvalidOperationException("开始菜单拒绝载入组件。错误代码 " + attach.ToString("X8"));
            int status = WaitForLayout(grid.Width * 256 + grid.Height);
            if (status == 4)
                throw new InvalidOperationException("当前显示器的空间放不下这些行列。");
            if (status >= 2 && status <= 6)
                throw new InvalidOperationException("当前开始菜单未能应用布局。状态 " + status);
            dynamic registered = RegisterLogonTask(folder);
            taskUpdated = true;
            registered.Run(null);
            return status == grid.Width * 256 + grid.Height
                ? string.Format("已应用 {0} 列 × {1} 行。以后登录自动生效。", grid.Width, grid.Height)
                : "已保存，今后登录会自动加载。请打开开始菜单查看。";
        }
        catch
        {
            if (previousWorker)
                RestartLoadedLayout(Path.Combine(installDirectory, LayoutFile));
            WriteGrid(previous);
            if (taskUpdated)
                previousTask = previousDefinition == null ? null : SaveLogonTask(folder, previousDefinition);
            if (taskUpdated && previousTask == null)
                RemoveLogonTask();
            else if (previousWorker && previousTask != null && !previous.IsEmpty)
                previousTask.Run(null);
            throw;
        }
    }

    internal void Restore()
    {
        if (Directory.Exists(profile))
            WriteGrid(Size.Empty);
        RemoveLogonTask();
        WaitForSessionExit();
        if (ReadLayoutStatus() > 6 && WaitForLayout(1) != 1)
            throw new InvalidOperationException("已关闭登录加载。请重新打开开始菜单查看默认布局。");
    }

    private static void WaitForSessionExit()
    {
        using (var worker = new Mutex(false, SessionLock))
        {
            bool finished;
            try { finished = worker.WaitOne(4500); }
            catch (AbandonedMutexException) { finished = true; }
            if (!finished)
                throw new InvalidOperationException("后台正在退出，请稍后再操作。");
            worker.ReleaseMutex();
        }
    }

    internal void RunSession()
    {
        if (!string.Equals(Path.GetFullPath(AppDomain.CurrentDomain.BaseDirectory).TrimEnd('\\'),
                           installDirectory, StringComparison.OrdinalIgnoreCase))
            return;
        CheckWindowsBuild();
        using (var mutex = new Mutex(false, SessionLock))
        {
            bool owner;
            try { owner = mutex.WaitOne(0); }
            catch (AbandonedMutexException) { owner = true; }
            if (!owner) return;
            try
            {
                while (!LoadGrid().IsEmpty)
                {
                    try
                    {
                        if (!LegacyGridLoaded())
                            TryAttach();
                    }
                    catch (Win32Exception) { }
                    catch (InvalidOperationException) { }
                    Thread.Sleep(3000);
                }
            }
            finally { mutex.ReleaseMutex(); }
        }
    }

    private void InstallBinaries()
    {
        string source = AppDomain.CurrentDomain.BaseDirectory;
        string packedLibrary = Path.Combine(source, LayoutFile);
        if (!File.Exists(packedLibrary))
            throw new InvalidOperationException("请先完整解压，再打开程序。");
        Directory.CreateDirectory(installDirectory);
        foreach (string name in new[] { ApplicationFile, LayoutFile })
        {
            string from = Path.Combine(source, name);
            string to = Path.Combine(installDirectory, name);
            if (!string.Equals(from, to, StringComparison.OrdinalIgnoreCase) && !File.Exists(to))
                File.Copy(from, to);
        }
    }

    private void WriteGrid(Size grid)
    {
        Directory.CreateDirectory(profile);
        string path = Path.Combine(profile, "grid.txt");
        string pending = Path.Combine(profile, "pending-" + Guid.NewGuid().ToString("N"));
        try
        {
            File.WriteAllText(pending, string.Format(CultureInfo.InvariantCulture, "{0} {1}\n", grid.Width, grid.Height), Encoding.ASCII);
            if (File.Exists(path))
                File.Replace(pending, path, null);
            else
                File.Move(pending, path);
        }
        finally
        {
            if (File.Exists(pending))
                File.Delete(pending);
        }
    }

    private void CheckWindowsBuild()
    {
        var path = Path.Combine(windows, StartMenuLocation);
        if (Environment.OSVersion.Version.Build == 22631 && File.Exists(path))
        {
            using (var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var sha256 = SHA256.Create())
            {
                var hash = BitConverter.ToString(sha256.ComputeHash(stream)).Replace("-", "");
                if (string.Equals(hash, StartMenuHash, StringComparison.Ordinal))
                    return;
            }
        }
        throw new InvalidOperationException("独立版适用于已验证的 Windows 11 23H2 x64 开始菜单组件。");
    }

    private Process FindStartHost()
    {
        string target = Path.Combine(windows, HostLocation);
        foreach (var process in Process.GetProcessesByName("StartMenuExperienceHost"))
        {
            try
            {
                if (process.SessionId == session && !process.HasExited &&
                    string.Equals(target, process.MainModule.FileName, StringComparison.OrdinalIgnoreCase))
                    return process;
            }
            catch (InvalidOperationException) { }
            catch (Win32Exception) { }
            process.Dispose();
        }
        return null;
    }

    private bool LegacyGridLoaded()
    {
        using (var process = FindStartHost())
        {
            if (process == null) return false;
            foreach (ProcessModule module in process.Modules)
                if (module.ModuleName.StartsWith("local@start-pinned-grid_", StringComparison.OrdinalIgnoreCase))
                    return true;
        }
        return false;
    }

    private string FindPreviousLayout(Process process)
    {
        if (process == null) return null;
        foreach (ProcessModule module in process.Modules)
        {
            string directory = Path.GetDirectoryName(module.FileName);
            if (string.Equals(module.ModuleName, LayoutFile, StringComparison.OrdinalIgnoreCase) &&
                string.Equals(Path.GetDirectoryName(directory), root, StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(directory, installDirectory, StringComparison.OrdinalIgnoreCase))
                return module.FileName;
        }
        return null;
    }

    private bool PreviousLayoutLoaded()
    {
        using (var process = FindStartHost())
            return FindPreviousLayout(process) != null;
    }

    private void RestartPreviousLayout()
    {
        using (var process = FindStartHost())
            if (FindPreviousLayout(process) != null)
                ExitStartHost(process);
    }

    private void RestartLoadedLayout(string layout)
    {
        using (var process = FindStartHost())
        {
            if (process == null) return;
            foreach (ProcessModule module in process.Modules)
            {
                if (!string.Equals(layout, module.FileName, StringComparison.OrdinalIgnoreCase)) continue;
                ExitStartHost(process);
                break;
            }
        }
    }

    private static void ExitStartHost(Process process)
    {
        process.Kill();
        if (!process.WaitForExit(3000))
            throw new InvalidOperationException("开始菜单正在重启，请稍后再操作。");
    }

    private int TryAttach()
    {
        string extension = Path.Combine(installDirectory, LayoutFile);
        using (var process = FindStartHost())
        {
            if (process == null) return MissingEndpoint;
            foreach (ProcessModule module in process.Modules)
                if (string.Equals(extension, module.FileName, StringComparison.OrdinalIgnoreCase))
                    return 0;
            for (int endpoint = 1; endpoint <= 32; endpoint++)
            {
                int result = InitializeXamlDiagnosticsEx("VisualDiagConnection" + endpoint,
                    (uint)process.Id, "", extension, TapClass, null);
                if (result != MissingEndpoint)
                    return result;
            }
        }
        return MissingEndpoint;
    }

    private int ReadLayoutStatus()
    {
        using (var process = FindStartHost())
        {
            if (process == null) return 0;
            int value = 0;
            foreach (ProcessThread thread in process.Threads)
            using (thread)
            {
                EnumThreadWindows((uint)thread.Id, (window, state) =>
                {
                    value = (int)GetProp(window, StatusProperty).ToInt64();
                    return value == 0;
                }, IntPtr.Zero);
                if (value != 0) return value;
            }
        }
        return 0;
    }

    private int WaitForLayout(int expected)
    {
        int status = 0;
        for (int attempt = 0; attempt < 25; attempt++)
        {
            status = ReadLayoutStatus();
            if (status == expected)
                return status;
            Thread.Sleep(120);
        }
        return status;
    }

    private static dynamic OpenTaskService()
    {
        dynamic service = Activator.CreateInstance(Type.GetTypeFromProgID("Schedule.Service", true));
        service.Connect();
        return service;
    }

    private dynamic FindLogonTask(dynamic folder)
    {
        try { return folder.GetTask(taskName); }
        catch (COMException error)
        {
            if (error.ErrorCode != unchecked((int)0x80070002)) throw;
            return null;
        }
    }

    private dynamic RegisterLogonTask(dynamic folder)
    {
        dynamic task = OpenTaskService().NewTask(0);
        task.RegistrationInfo.Description = "为当前用户在登录时加载开始菜单网格。";
        task.Principal.UserId = user;
        task.Principal.LogonType = 3; // TASK_LOGON_INTERACTIVE_TOKEN
        task.Principal.RunLevel = 1; // TASK_RUNLEVEL_HIGHEST
        task.Settings.StartWhenAvailable = true;
        task.Settings.MultipleInstances = 2; // TASK_INSTANCES_IGNORE_NEW
        task.Settings.DisallowStartIfOnBatteries = false;
        task.Settings.StopIfGoingOnBatteries = false;
        task.Settings.ExecutionTimeLimit = "PT0S";
        dynamic trigger = task.Triggers.Create(9); // TASK_TRIGGER_LOGON
        trigger.UserId = user;
        dynamic action = task.Actions.Create(0); // TASK_ACTION_EXEC
        action.Path = Path.Combine(installDirectory, ApplicationFile);
        action.Arguments = "--session";
        action.WorkingDirectory = installDirectory;
        return SaveLogonTask(folder, task);
    }

    private dynamic SaveLogonTask(dynamic folder, dynamic task)
    {
        return folder.RegisterTaskDefinition(taskName, task, 6, user, null, 3, null);
    }

    private void RemoveLogonTask()
    {
        try
        {
            dynamic service = OpenTaskService();
            dynamic folder = service.GetFolder(@"\");
            folder.DeleteTask(taskName, 0);
        }
        catch (COMException error)
        {
            if (error.ErrorCode != unchecked((int)0x80070002))
                throw;
        }
    }
}
