using Newtonsoft.Json.Linq;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using static RuntimeSwitcher.AppListForm;

namespace RuntimeSwitcher
{

    public partial class MainWindow : Form
    {
        private static readonly Func<string, bool> STEAMVR_TEST = s => s.Contains("SteamVR");

        private ConfigReader config;
        private string ocRuntimePath;
        private string revisionFilePath;
        private string defaultVersionFilePath;
        private string appListPath;
        private string appConfigPath;
        private string ocNewVersionPath;

        // Existing members...
        private readonly JObject _globalConfig;
        private readonly Dictionary<string, string> _directoryList = new Dictionary<string, string>();
        private string KeyDefaultVersion;

        private SemaphoreSlim dllDownloadMutex = new SemaphoreSlim(1);

        private AppListForm appList;

        public MainWindow()
        {
            ocRuntimePath = Directory.GetParent(System.Reflection.Assembly.GetEntryAssembly().Location).FullName;
            ocRuntimePath += Path.DirectorySeparatorChar + "Runtime";
            revisionFilePath = ocRuntimePath + Path.DirectorySeparatorChar + "revision.txt";
            defaultVersionFilePath = ocRuntimePath + Path.DirectorySeparatorChar + "defaultVersion.txt";
            appListPath = ocRuntimePath + Path.DirectorySeparatorChar + "applist.json";
            appConfigPath = ocRuntimePath + Path.DirectorySeparatorChar + "apps-config.json";
            ocNewVersionPath = ocRuntimePath;

            if (!Directory.Exists(ocRuntimePath)) {
                Directory.CreateDirectory(ocRuntimePath);
            }

            config = new ConfigReader();

            InitializeComponent();

            // Initialize and populate the defaultVersion dropdown

            PopulateDirectoryList();

            UpdateDefaultVersionList();            

            // Set the selected index based on saved configuration
            SetDefaultVersionDropdown();           

            UpdateStatus();

            if(File.Exists(revisionFilePath))
            {
                CheckForUpdate();
            }
        }
        private void PopulateDirectoryList()
        {
            try
            {
                // Ensure the directory exists
                if (Directory.Exists(ocRuntimePath))
                {
                    foreach (string directoryPath in Directory.GetDirectories(ocRuntimePath))
                    {
                        DirectoryInfo dirInfo = new DirectoryInfo(directoryPath);
                        string dirName = dirInfo.Name;

                        _directoryList[dirName] = dirName;
                    }
                }
            }
            catch (Exception ex)
            {
                // Handle any exceptions, e.g., log or show an error message
                Console.WriteLine("Error: " + ex.Message);
            }
        }

        private void UpdateStatus()
        {
            string runtime = config.Runtimes.Count == 0 ? null : config.Runtimes.First();
            useOpenComposite.Enabled = true;

            useSteamVR.Enabled = config.Runtimes.Where(STEAMVR_TEST).Count() > 0;

            if (runtime == null)
            {
                statusLabel.Text = "None";
            }
            else if (STEAMVR_TEST.Invoke(runtime))
            {
                statusLabel.Text = "SteamVR";
                useSteamVR.Enabled = false;
            }
            else if (runtime.StartsWith(ocRuntimePath))
            {
                statusLabel.Text = "OpenComposite (OpenXR): " + runtime.Substring(ocRuntimePath.Length + 1);
                useOpenComposite.Enabled = false;
            }
            else
            {
                statusLabel.Text = "Other";
            }
        }

        private async void CheckForUpdate()
        {
            string currentRevision = File.ReadAllText(revisionFilePath).Trim();

            updatesLabel.Visible = true;
            updatesLabel.Text = "Checking for updates...";

            string serverRevision = await UpdateChecker.GetLatestHash();

            if(serverRevision != currentRevision)
            {
                updatesLabel.Text = "Update Found!";
                doUpdate.Visible = true;

                ocNewVersionPath = ocRuntimePath + Path.DirectorySeparatorChar + serverRevision;
                if (!Directory.Exists(ocNewVersionPath))
                {
                    Directory.CreateDirectory(ocNewVersionPath);
                    if (!Directory.Exists(ocNewVersionPath + Path.DirectorySeparatorChar + "bin"))
                    {
                        Directory.CreateDirectory(ocNewVersionPath + Path.DirectorySeparatorChar + "bin");
                    }
                }
            } else
            {
                updatesLabel.Text = "Already up-to-date";
            }
        }

        private async void doUpdate_Click(object sender, EventArgs e)
        {
            doUpdate.Enabled = false;
            updatesLabel.Text = "Updating...";

            await UpdateDLLs();

            doUpdate.Visible = false;
            updatesLabel.Text = "Update Complete";
        }

        private void UpdatePathsFile(string version)
        {
            List<string> rts = new List<string>(config.Runtimes);
            rts.RemoveAll(s => s.Contains("OpenComposite"));
            rts.Insert(0, version);
            config.Runtimes = rts;

            UpdateStatus();
        }

        private void SwitchToOpenComposite()
        {            
            UpdatePathsFile(ocNewVersionPath);

            // Copy the 'version.txt' file from SteamVR's bin directory to OpenComposite's bin directory
            //  this stops the Steam client from automatically changing this
            //  credit to ArtyDidNothingWrong for finding this:
            //  see https://www.reddit.com/r/oculus/comments/9nxixe/systemwide_installation_for_opencomposite_released/e7qg27q/

            // Check SteamVR is installed, otherwise we don't need to bother with this
            List<string> rts = new List<string>(config.Runtimes);
            string svrPath = rts.FirstOrDefault(STEAMVR_TEST);
            if (string.IsNullOrEmpty(svrPath))
                return;

            string svrVersionFile = Path.Combine(svrPath, "bin", "version.txt");
            string ocVersionFile = Path.Combine(ocNewVersionPath, "version.txt");

            // If the SteamVR version file is missing (eg a corrupted SteamVR installation), stop here.
            if(!File.Exists(svrVersionFile))
                return;

            File.Copy(svrVersionFile, ocVersionFile, true);
        }

        private async Task<bool> UpdateDLLs()
        {
            await dllDownloadMutex.WaitAsync().ConfigureAwait(false);

            bool downloads = false;

            string version = await UpdateChecker.GetLatestHash();
            string dateVersion = DateTime.Now.ToString("yyyy-MM-dd");

            ocNewVersionPath = ocRuntimePath + Path.DirectorySeparatorChar + dateVersion;
            if (!Directory.Exists(ocNewVersionPath + Path.DirectorySeparatorChar + "bin"))
            {
                Directory.CreateDirectory(ocNewVersionPath + Path.DirectorySeparatorChar + "bin");
            }

            string vrclient = ocNewVersionPath + Path.DirectorySeparatorChar + "bin" + Path.DirectorySeparatorChar + "vrclient.dll";
            if (!File.Exists(vrclient))
            {
                downloads = true;
                statusLabel.Text = "Downloading 32-bit DLL";
                await DownloadFile(vrclient, "https://znix.xyz/OpenComposite/download.php?arch=x86&branch=openxr");
            }

            string vrclient_x64 = ocNewVersionPath + Path.DirectorySeparatorChar + "bin" + Path.DirectorySeparatorChar + "vrclient_x64.dll";
            if (!File.Exists(vrclient_x64))
            {
                downloads = true;
                statusLabel.Text = "Downloading 64-bit DLL";
                await DownloadFile(vrclient_x64, "https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr");
            }

            if(downloads)
            {
                File.WriteAllText(revisionFilePath, version);
                File.WriteAllText(defaultVersionFilePath, dateVersion);
            }
                

            dllDownloadMutex.Release();

            return downloads;
        }

        private async void useOpenComposite_Click(object sender, EventArgs e)
        {
            useOpenComposite.Enabled = false;

            bool downloads = await UpdateDLLs();

            SwitchToOpenComposite();

            if(downloads)
            {
                statusLabel.Text += " (Download Complete)";
            }
        }

        private async Task DownloadFile(string name, string url)
        {
            using (WebClient wc = new WebClient())
            {
                progressBar.Visible = true;

                wc.DownloadProgressChanged += (object sender, DownloadProgressChangedEventArgs e) =>
                {
                    progressBar.Value = e.ProgressPercentage;
                };

                // Ensure the file gets redownloaded if the program is closed during download
                string temp = name + ".part";

                await wc.DownloadFileTaskAsync(
                    // Param1 = Link of file
                    new System.Uri(url),
                    // Param2 = Path to save
                    temp
                );

                File.Move(temp, name);

                progressBar.Value = 0;
                progressBar.Visible = false;
            }
        }

        private void useSteamVR_Click(object sender, EventArgs e)
        {
            List<string> rts = new List<string>(config.Runtimes);

            // Remove OpenComposite
            rts.RemoveAll(s => s == ocRuntimePath);

            // Move SteamVR to the top of the list
            string svrPath = rts.FirstOrDefault(STEAMVR_TEST);
            if (!string.IsNullOrEmpty(svrPath))
            {
                rts.Clear();
                rts.Insert(0, svrPath);
            }

            // Apply the changes and update the UI
            config.Runtimes = rts;
            UpdateStatus();
        }

        private void configureApps_Click(object sender, EventArgs e)
        {
            if (appList != null && !appList.IsDisposed)
            {
                appList.Focus();
                return;
            }

            appList = new AppListForm(appListPath, appConfigPath);
            appList.Show(this);
        }

        private void Form_Load(object sender, EventArgs e)
        {
            UpdateDefaultVersionList();
        }

        private void UpdateDefaultVersionList()
        {
            try
            {
                defaultVersion.Items.Clear();
                foreach (string dir in _directoryList.Values)
                {
                    defaultVersion.Items.Add(dir);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Error: " + ex.Message);
            }
        }

        private void SetDefaultVersionDropdown()
        {
            if (!File.Exists(defaultVersionFilePath) && _directoryList.Count() > 0)
            {
                string newDefaultVersion = _directoryList.First().Key;
                File.WriteAllText(defaultVersionFilePath, newDefaultVersion);
                KeyDefaultVersion = newDefaultVersion;
            } else
            {
                if (!File.Exists(defaultVersionFilePath))
                {
                    return;                    
                }
                KeyDefaultVersion = File.ReadAllText(defaultVersionFilePath).Trim();
            }

            if (_directoryList.ContainsKey(KeyDefaultVersion))
            {
                // Assuming the saved version is just a string (directory name)
                string savedVersion = _directoryList[KeyDefaultVersion].ToString();

                int count = 0;
                foreach (var item in defaultVersion.Items)
                {
                    // Assuming the items in defaultVersion are strings
                    if (item.ToString() == savedVersion)
                    {
                        defaultVersion.SelectedIndex = count;
                        break;
                    }
                    count++;
                }
            }
        }

        private void defaultVersion_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (defaultVersion.SelectedIndex != -1)
            {
                string selectedFolderName = defaultVersion.SelectedItem.ToString();
                UpdatePathsFile(ocRuntimePath + Path.DirectorySeparatorChar + selectedFolderName);
            }
        }

        private void MainWindow_Load(object sender, EventArgs e)
        {

        }

        private void label3_Click(object sender, EventArgs e)
        {

        }

        private void defaultRuntime_SelectedIndexChanged(object sender, EventArgs e)
        {

        }
    }
}
