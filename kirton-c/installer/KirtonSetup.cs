// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0/.
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Collections.Generic;
using System.Reflection;
using System.Windows.Forms;

internal static class KirtonSetup
{
    private const string SetupSettingsResource = "SetupSettings";
    private static readonly string[] SetupSettings = ReadSetupSettings();
    private static string Version { get { return SetupSettings[0]; } }
    private static string SdkName { get { return SetupSettings[1]; } }
    private static string ProductName { get { return SetupSettings[2]; } }
    private static bool IsPreview { get { return Version.IndexOf("preview", StringComparison.OrdinalIgnoreCase) >= 0; } }
    private static string uiLanguage = "ru";
    private const string SdkArchiveResource = "KirtonSdkZip";
    private const string LicenseResource = "MplLicense";
    private const string InstallScriptResource = "InstallScript";
    private const string UninstallScriptResource = "UninstallScript";

    private static string T(string russian, string english)
    {
        return uiLanguage == "en" ? english : russian;
    }

    private static string[] ReadSetupSettings()
    {
        using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(SetupSettingsResource))
        {
            if (stream == null) throw new InvalidOperationException("Setup metadata resource is missing.");
            using (StreamReader reader = new StreamReader(stream))
            {
                string[] settings = reader.ReadToEnd().Replace("\r", "").Split('\n');
                if (settings.Length < 5 || string.IsNullOrWhiteSpace(settings[0]) ||
                    string.IsNullOrWhiteSpace(settings[1]) || string.IsNullOrWhiteSpace(settings[2]))
                    throw new InvalidOperationException("Setup metadata is invalid.");
                return settings;
            }
        }
    }

    [STAThread]
    private static int Main(string[] args)
    {
        bool smokeTest = args.Length == 2 && string.Equals(args[0], "--smoke-test", StringComparison.OrdinalIgnoreCase);
        try
        {
            if (smokeTest)
                return RunSmokeTest(args[1]);

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupWizard());
            return 0;
        }
        catch (Exception ex)
        {
            if (smokeTest)
            {
                Console.Error.WriteLine(ex.ToString());
                try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "KirtonSetup-smoke-error.txt"), ex.ToString()); } catch { }
            }
            else MessageBox.Show(ex.Message, ProductName + " — " + T("ошибка установки", "setup error"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    private static int RunSmokeTest(string testRoot)
    {
        string target = Path.Combine(Path.GetFullPath(testRoot), "Install");
        Environment.SetEnvironmentVariable("KIRTON_SETUP_TEST", "1", EnvironmentVariableTarget.Process);
        string smokeLanguage = Version.IndexOf("preview", StringComparison.OrdinalIgnoreCase) >= 0 ? "ru" : "en";
        try { RunInstallerScript(target, false, false, smokeLanguage); }
        finally { Environment.SetEnvironmentVariable("KIRTON_SETUP_TEST", null, EnvironmentVariableTarget.Process); }
        string executable = Path.Combine(target, "bin", "kirton.exe");
        if (!File.Exists(executable) || !File.Exists(Path.Combine(target, "LICENSE")) ||
            !File.Exists(Path.Combine(target, "THIRD_PARTY_NOTICES.md")))
            throw new InvalidOperationException("The setup package is missing SDK or license files.");

        string version = RunProgram(executable, "--version", target);
        if (version.IndexOf("Kirton " + Version, StringComparison.Ordinal) < 0)
            throw new InvalidOperationException("The installed interpreter did not report the expected version.");
        string script = Path.Combine(target, "installer-smoke.kir");
        File.WriteAllText(script, "\u043f\u0435\u0447\u0430\u0442\u0430\u0442\u044c(\"INSTALL_OK\")\r\n");
        string astOutput = RunProgram(executable, "\"" + script + "\"", target);
        string bytecodeOutput = RunProgram(executable, "--bytecode \"" + script + "\"", target);
        if (astOutput.IndexOf("INSTALL_OK", StringComparison.Ordinal) < 0 ||
            bytecodeOutput.IndexOf("INSTALL_OK", StringComparison.Ordinal) < 0)
            throw new InvalidOperationException("AST/Bytecode installed smoke did not print its marker.");

        string project = Path.Combine(target, "my-project.kir");
        File.WriteAllText(project, "# user project preserved by uninstall\r\n");
        Environment.SetEnvironmentVariable("KIRTON_UNINSTALL_SILENT", "1", EnvironmentVariableTarget.Process);
        RunPowerShell(Path.Combine(target, "Uninstall-Kirton.ps1"), "", target);
        Environment.SetEnvironmentVariable("KIRTON_UNINSTALL_SILENT", null, EnvironmentVariableTarget.Process);
        if (!File.Exists(project)) throw new InvalidOperationException("The uninstaller removed a user project.");
        return 0;
    }

    private static void RunInstallerScript(string installDir, bool addPath, bool createShortcuts, string language)
    {
        string temp = Path.Combine(Path.GetTempPath(), "kirton-setup-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            CopyResource(SdkArchiveResource, Path.Combine(temp, SdkName + ".zip"));
            CopyResource(LicenseResource, Path.Combine(temp, "LICENSE"));
            CopyResource(InstallScriptResource, Path.Combine(temp, "Install-Kirton.ps1"));
            CopyResource(UninstallScriptResource, Path.Combine(temp, "Uninstall-Kirton.ps1"));
            CopyResource(SetupSettingsResource, Path.Combine(temp, "SetupSettings.txt"));
            string script = Path.Combine(temp, "Install-Kirton.ps1");
            string arguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(script) +
                " -InstallDir " + Quote(Path.GetFullPath(installDir)) + " -Silent -Language " + Quote(language);
            if (addPath) arguments += " -AddPath";
            if (createShortcuts) arguments += " -CreateShortcuts";
            RunPowerShell(script, arguments, temp, false);
        }
        finally
        {
            try { Directory.Delete(temp, true); } catch { }
        }
    }

    private static void CopyResource(string resourceName, string destination)
    {
        using (Stream input = Assembly.GetExecutingAssembly().GetManifestResourceStream(resourceName))
        {
            if (input == null) throw new InvalidOperationException("Setup resource is missing: " + resourceName);
            using (FileStream output = File.Create(destination)) input.CopyTo(output);
        }
    }

    private static string RunProgram(string fileName, string arguments, string workingDirectory)
    {
        ProcessStartInfo info = new ProcessStartInfo(fileName, arguments);
        info.WorkingDirectory = workingDirectory;
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        using (Process process = Process.Start(info))
        {
            string output = process.StandardOutput.ReadToEnd();
            string error = process.StandardError.ReadToEnd();
            process.WaitForExit();
            if (process.ExitCode != 0) throw new InvalidOperationException(error.Length == 0 ? output : error);
            return output;
        }
    }

    private static void RunPowerShell(string scriptPath, string arguments, string workingDirectory, bool addScriptArgument = true)
    {
        string powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
            @"WindowsPowerShell\v1.0\powershell.exe");
        if (addScriptArgument)
            arguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(scriptPath) + " " + arguments;
        string output = RunProgram(powershell, arguments, workingDirectory);
        if (!string.IsNullOrWhiteSpace(output)) Debug.WriteLine(output);
    }

    private static string Quote(string value)
    {
        return "\"" + value.Replace("\"", "\\\"") + "\"";
    }

    private sealed class SetupWizard : Form
    {
        private readonly Panel[] pages = new Panel[5];
        private readonly Dictionary<Control, string[]> localized = new Dictionary<Control, string[]>();
        private readonly Button backButton;
        private readonly Button nextButton;
        private readonly Button cancelButton;
        private readonly CheckBox acceptLicense;
        private readonly TextBox licenseBox;
        private readonly TextBox installPath;
        private readonly ComboBox languageChoice;
        private readonly CheckBox addPath;
        private readonly CheckBox shortcuts;
        private readonly Label progressText;
        private readonly ProgressBar progress;
        private readonly Label finishText;
        private readonly BackgroundWorker worker;
        private int pageIndex;
        private bool busy;
        private bool changingLanguage;
        private bool installComplete;

        public SetupWizard()
        {
            Text = KirtonSetup.ProductName + " — мастер установки";
            ClientSize = new Size(820, 620);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.White;
            Font = new Font("Segoe UI", 9F);
            Icon = SystemIcons.Application;

            Panel banner = new Panel();
            banner.Dock = DockStyle.Top;
            banner.Height = 112;
            banner.BackColor = Color.FromArgb(31, 48, 78);
            Controls.Add(banner);
            Label brand = new Label();
            brand.Text = "K";
            brand.Location = new Point(26, 22);
            brand.Size = new Size(58, 58);
            brand.TextAlign = ContentAlignment.MiddleCenter;
            brand.Font = new Font("Segoe UI Semibold", 28F);
            brand.ForeColor = Color.White;
            brand.BackColor = Color.FromArgb(0, 145, 150);
            banner.Controls.Add(brand);
            Label title = new Label();
            title.Text = KirtonSetup.ProductName;
            title.Location = new Point(104, 17);
            title.Size = new Size(680, 34);
            title.Font = new Font("Segoe UI Semibold", 18F);
            title.ForeColor = Color.White;
            banner.Controls.Add(title);
            Label subtitle = new Label();
            SetText(subtitle, IsPreview ? "Установщик Preview для Windows x64" : "Стабильный установщик Windows x64",
                IsPreview ? "Preview installer for Windows x64" : "Stable installer for Windows x64");
            subtitle.Location = new Point(106, 53);
            subtitle.Size = new Size(680, 22);
            subtitle.ForeColor = Color.FromArgb(220, 229, 240);
            banner.Controls.Add(subtitle);

            Label publisher = new Label();
            SetText(publisher, "Издатель: WorldCraft Studio", "Publisher: WorldCraft Studio");
            publisher.Location = new Point(106, 77);
            publisher.Size = new Size(680, 20);
            publisher.ForeColor = Color.FromArgb(220, 229, 240);
            banner.Controls.Add(publisher);

            Panel content = new Panel();
            content.Location = new Point(28, 126);
            content.Size = new Size(764, 432);
            content.BackColor = Color.White;
            Controls.Add(content);
            for (int i = 0; i < pages.Length; i++)
            {
                pages[i] = new Panel();
                pages[i].Dock = DockStyle.Fill;
                pages[i].Visible = false;
                content.Controls.Add(pages[i]);
            }

            AddHeading(
                pages[0],
                "Добро пожаловать в мастер установки Kirton Compiler " + Version,
                "Welcome to the Kirton Compiler " + Version + " installation wizard",
                IsPreview ? "Это Kirton Compiler " + Version + " Preview для Windows x64. Выберите язык интерфейса." : "Это Kirton Compiler " + Version + " Stable для Windows x64. Выберите язык интерфейса.",
                IsPreview ? "This is Kirton Compiler " + Version + " Preview for Windows x64. Choose the interface language." : "This is Kirton Compiler " + Version + " Stable for Windows x64. Choose the interface language.");
            AddLocalizedLabel(pages[0], "Язык интерфейса:", "Interface language:", 4, 105, 130, 28);
            languageChoice = new ComboBox();
            languageChoice.DropDownStyle = ComboBoxStyle.DropDownList;
            languageChoice.Location = new Point(142, 102);
            languageChoice.Size = new Size(220, 28);
            languageChoice.Items.Add("Русский");
            languageChoice.Items.Add("Английский");
            languageChoice.SelectedIndex = Version.IndexOf("preview", StringComparison.OrdinalIgnoreCase) >= 0 ? 0 : 1;
            pages[0].Controls.Add(languageChoice);
            languageChoice.SelectedIndexChanged += delegate { if (!changingLanguage) ApplyLanguage(); };

            Label welcome = AddLocalizedLabel(
                pages[0],
                "В установщик входят:\r\n\r\n• Интерпретатор Kirton\r\n• Байткодовый компилятор и VM\r\n• Стандартные библиотеки\r\n• C API и заголовочные файлы\r\n• Примеры и документация",
                "This installer includes:\r\n\r\n• Kirton interpreter\r\n• Bytecode compiler and VM\r\n• Standard libraries\r\n• C API and header files\r\n• Examples and documentation",
                4, 148, 744, 240);
            welcome.Font = new Font("Segoe UI", 11F);

            AddHeading(pages[1], "Лицензионное соглашение", "License agreement",
                "Ниже приведён официальный текст Mozilla Public License 2.0.",
                "The official Mozilla Public License 2.0 text is shown below.");
            licenseBox = new TextBox();
            licenseBox.Multiline = true;
            licenseBox.ReadOnly = true;
            licenseBox.ScrollBars = ScrollBars.Vertical;
            licenseBox.WordWrap = false;
            licenseBox.Font = new Font("Consolas", 8.5F);
            licenseBox.Location = new Point(4, 100);
            licenseBox.Size = new Size(746, 248);
            licenseBox.Text = ReadResourceText(LicenseResource);
            pages[1].Controls.Add(licenseBox);
            acceptLicense = new CheckBox();
            SetText(acceptLicense,
                "Я прочитал(а) и принимаю Mozilla Public License 2.0.",
                "I have read and accept the Mozilla Public License 2.0.");
            acceptLicense.Location = new Point(4, 356);
            acceptLicense.Size = new Size(746, 34);
            pages[1].Controls.Add(acceptLicense);
            acceptLicense.CheckedChanged += delegate { RefreshPage(); };

            AddHeading(pages[2], "Параметры установки", "Choose install options",
                IsPreview ? "Preview устанавливается отдельно и не заменяет другие версии Kirton." : "Установка сохраняет посторонние файлы и существующие версии Kirton.",
                IsPreview ? "This Preview installs separately and does not replace other Kirton versions." : "Unrelated files and existing Kirton versions are preserved.");
            AddLocalizedLabel(pages[2], "Каталог установки:", "Install location:", 4, 105, 620, 26);
            installPath = new TextBox();
            installPath.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", KirtonSetup.ProductName);
            installPath.Location = new Point(4, 134);
            installPath.Size = new Size(618, 28);
            pages[2].Controls.Add(installPath);
            Button browse = new Button();
            SetText(browse, "Обзор...", "Browse...");
            browse.Location = new Point(632, 132);
            browse.Size = new Size(118, 30);
            pages[2].Controls.Add(browse);
            browse.Click += delegate
            {
                using (FolderBrowserDialog dialog = new FolderBrowserDialog())
                {
                    dialog.Description = T("Выберите каталог для " + ProductName + ".", "Choose a folder for " + ProductName + ".");
                    dialog.SelectedPath = installPath.Text;
                    dialog.ShowNewFolderButton = true;
                    if (dialog.ShowDialog(this) == DialogResult.OK) installPath.Text = dialog.SelectedPath;
                }
            };
            addPath = new CheckBox();
            SetText(addPath, "Добавить каталог bin в пользовательский PATH", "Add the bin directory to my user PATH");
            addPath.Checked = true;
            addPath.Location = new Point(4, 184);
            addPath.Size = new Size(746, 28);
            pages[2].Controls.Add(addPath);
            shortcuts = new CheckBox();
            SetText(shortcuts, "Создать ярлыки меню «Пуск» для документации и удаления", "Create Start menu shortcuts for documentation and uninstall");
            shortcuts.Checked = true;
            shortcuts.Location = new Point(4, 220);
            shortcuts.Size = new Size(746, 28);
            pages[2].Controls.Add(shortcuts);
            Label note = AddLocalizedLabel(pages[2],
                "Установщик сохраняет посторонние файлы и регистрирует отдельное удаление.",
                "Unrelated files are preserved; this product gets its own uninstall entry.",
                4, 264, 746, 58);
            note.ForeColor = Color.FromArgb(75, 86, 102);

            AddHeading(pages[3], "Установка " + ProductName, "Installing " + ProductName,
                "Пожалуйста, дождитесь завершения установки.", "Please wait while setup installs the Preview.");
            progressText = AddLocalizedLabel(pages[3], "Подготовка установки...", "Preparing installation...", 4, 138, 746, 30);
            progress = new ProgressBar();
            progress.Location = new Point(4, 180);
            progress.Size = new Size(746, 24);
            progress.Style = ProgressBarStyle.Marquee;
            progress.MarqueeAnimationSpeed = 25;
            pages[3].Controls.Add(progress);

            AddHeading(pages[4], "Установка завершена", "Installation complete",
                ProductName + " готов к работе.", ProductName + " is ready to use.");
            finishText = AddLocalizedLabel(pages[4],
                "Новый терминал: kirton --version\r\n\r\nДокументация и примеры находятся в каталоге установки.",
                "Open a new terminal and run: kirton --version\r\n\r\nDocumentation and examples are in the install directory.",
                4, 136, 746, 160);
            finishText.Font = new Font("Segoe UI", 10.5F);

            Panel footer = new Panel();
            footer.Dock = DockStyle.Bottom;
            footer.Height = 62;
            footer.BackColor = Color.FromArgb(246, 248, 251);
            Controls.Add(footer);
            backButton = new Button();
            SetText(backButton, "Назад", "Back");
            backButton.Location = new Point(500, 14);
            backButton.Size = new Size(88, 34);
            footer.Controls.Add(backButton);
            nextButton = new Button();
            SetText(nextButton, "Далее", "Next");
            nextButton.Location = new Point(596, 14);
            nextButton.Size = new Size(104, 34);
            nextButton.BackColor = Color.FromArgb(0, 145, 150);
            nextButton.ForeColor = Color.White;
            footer.Controls.Add(nextButton);
            cancelButton = new Button();
            SetText(cancelButton, "Отмена", "Cancel");
            cancelButton.Location = new Point(708, 14);
            cancelButton.Size = new Size(92, 34);
            footer.Controls.Add(cancelButton);

            backButton.Click += delegate { if (pageIndex > 0 && pageIndex < 3) { pageIndex--; RefreshPage(); } };
            cancelButton.Click += delegate { if (!busy) Close(); };
            nextButton.Click += NextClicked;
            FormClosing += delegate(object sender, FormClosingEventArgs e) { if (busy) e.Cancel = true; };

            worker = new BackgroundWorker();
            worker.DoWork += delegate(object sender, DoWorkEventArgs e)
            {
                object[] options = (object[])e.Argument;
                RunInstallerScript((string)options[0], (bool)options[1], (bool)options[2], (string)options[3]);
            };
            worker.RunWorkerCompleted += delegate(object sender, RunWorkerCompletedEventArgs e)
            {
                busy = false;
                UseWaitCursor = false;
                if (e.Error != null)
                {
                    MessageBox.Show(
                        T("Установка не завершена. Подробности:", "Installation did not complete. Details:") + "\r\n\r\n" +
                            (e.Error.InnerException == null ? e.Error.Message : e.Error.InnerException.Message),
                        KirtonSetup.ProductName, MessageBoxButtons.OK, MessageBoxIcon.Error);
                    pageIndex = 2;
                    RefreshPage();
                    return;
                }
                installComplete = true;
                finishText.Text = CompletionText();
                pageIndex = 4;
                RefreshPage();
            };
            ApplyLanguage();
        }

        private void SetText(Control control, string russian, string english)
        {
            localized[control] = new string[] { russian, english };
            control.Text = russian;
        }

        private Label AddLocalizedLabel(Control parent, string russian, string english,
            int x, int y, int width, int height)
        {
            Label label = new Label();
            SetText(label, russian, english);
            label.Location = new Point(x, y);
            label.Size = new Size(width, height);
            label.ForeColor = Color.FromArgb(75, 86, 102);
            parent.Controls.Add(label);
            return label;
        }

        private void AddHeading(Control parent, string russianTitle, string englishTitle,
            string russianDescription, string englishDescription)
        {
            Label heading = new Label();
            SetText(heading, russianTitle, englishTitle);
            heading.Location = new Point(2, 0);
            heading.Size = new Size(756, 54);
            heading.Font = new Font("Segoe UI Semibold", 15F);
            heading.ForeColor = Color.FromArgb(31, 48, 78);
            parent.Controls.Add(heading);
            AddLocalizedLabel(parent, russianDescription, englishDescription, 4, 56, 746, 40);
        }

        private static string T(string russian, string english)
        {
            return uiLanguage == "en" ? english : russian;
        }

        private string CompletionText()
        {
            return T("Установка завершена.\r\n\r\nКаталог: ", "Installation complete.\r\n\r\nLocation: ") +
                installPath.Text.Trim() + T("\r\n\r\nОткройте новый терминал и выполните: kirton --version\r\nДокументация и примеры находятся в каталоге установки.",
                    "\r\n\r\nOpen a new terminal and run: kirton --version\r\nDocumentation and examples are in the install directory.");
        }

        private void ApplyLanguage()
        {
            int languageIndex = languageChoice.SelectedIndex == 1 ? 1 : 0;
            uiLanguage = languageIndex == 1 ? "en" : "ru";
            foreach (KeyValuePair<Control, string[]> item in localized)
                item.Key.Text = item.Value[languageIndex];

            changingLanguage = true;
            languageChoice.BeginUpdate();
            languageChoice.Items.Clear();
            languageChoice.Items.Add(languageIndex == 1 ? "Russian" : "Русский");
            languageChoice.Items.Add(languageIndex == 1 ? "English" : "Английский");
            languageChoice.SelectedIndex = languageIndex;
            languageChoice.EndUpdate();
            changingLanguage = false;

            Text = KirtonSetup.ProductName + " — " + T("мастер установки", "Setup Wizard");
            if (installComplete) finishText.Text = CompletionText();
            RefreshPage();
        }

        private static string ReadResourceText(string name)
        {
            using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
            using (StreamReader reader = new StreamReader(stream))
            {
                return reader.ReadToEnd().Replace("\r\n", "\n").Replace("\n", "\r\n");
            }
        }

        private void RefreshPage()
        {
            for (int i = 0; i < pages.Length; i++) pages[i].Visible = (i == pageIndex);
            backButton.Visible = (pageIndex > 0 && pageIndex < 3);
            cancelButton.Visible = (pageIndex < 3);
            nextButton.Visible = (pageIndex != 3);
            nextButton.Enabled = true;
            if (pageIndex == 0) nextButton.Text = T("Далее", "Next");
            else if (pageIndex == 1) { nextButton.Text = T("Далее", "Next"); nextButton.Enabled = acceptLicense.Checked; }
            else if (pageIndex == 2) { nextButton.Text = T("Установить", "Install"); nextButton.Enabled = !string.IsNullOrWhiteSpace(installPath.Text); }
            else if (pageIndex == 3) { nextButton.Visible = false; }
            else { nextButton.Text = T("Готово", "Finish"); }
            backButton.Text = T("Назад", "Back");
            cancelButton.Text = T("Отмена", "Cancel");
        }

        private void NextClicked(object sender, EventArgs e)
        {
            if (pageIndex == 4) { Close(); return; }
            if (pageIndex < 2) { pageIndex++; RefreshPage(); return; }
            if (pageIndex != 2) return;

            string destination;
            try { destination = Path.GetFullPath(installPath.Text.Trim()); }
            catch
            {
                MessageBox.Show(T("Укажите корректный каталог установки.", "Enter a valid install folder."),
                    KirtonSetup.ProductName, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (File.Exists(destination))
            {
                MessageBox.Show(T("Указанный путь ведёт к файлу. Выберите каталог.", "The selected path is a file. Choose a folder."),
                    KirtonSetup.ProductName, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (Directory.Exists(destination) && Directory.GetFileSystemEntries(destination).Length > 0 &&
                !File.Exists(Path.Combine(destination, ".kirton-installed-files.txt")))
            {
                DialogResult answer = MessageBox.Show(
                    T("Каталог не является установкой Kirton. Файлы SDK с совпадающими именами могут быть заменены; посторонние файлы сохранятся.\r\n\r\n",
                        "This folder is not a Kirton installation. Files with matching SDK names may be replaced; unrelated files are preserved.\r\n\r\n") +
                        destination + "\r\n\r\n" + T("Продолжить?", "Continue?"),
                    KirtonSetup.ProductName, MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (answer != DialogResult.Yes) return;
            }

            installPath.Text = destination;
            pageIndex = 3;
            busy = true;
            UseWaitCursor = true;
            progressText.Text = T("Установка файлов SDK...", "Installing SDK files...");
            RefreshPage();
            string language = languageChoice.SelectedIndex == 1 ? "en" : "ru";
            worker.RunWorkerAsync(new object[] { destination, addPath.Checked, shortcuts.Checked, language });
        }
    }
}
