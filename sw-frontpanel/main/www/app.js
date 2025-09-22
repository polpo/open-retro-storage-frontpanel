let currentDisc = "No disc loaded";

// API helper function
async function apiCall(endpoint, options = {}) {
    try {
        const response = await fetch('/api' + endpoint, {
            headers: {
                'Content-Type': 'application/json',
                ...options.headers
            },
            ...options
        });

        if (!response.ok) {
            throw new Error(`HTTP ${response.status}: ${response.statusText}`);
        }

        return await response.json();
    } catch (error) {
        console.error('API call failed:', error);
        showStatus('error', `API Error: ${error.message}`);
        return null;
    }
}

function showStatus(type, message) {
    const statusDiv = document.getElementById('connection-status');
    statusDiv.className = `status ${type}`;
    statusDiv.textContent = message;
}

async function loadSystemInfo() {
    const data = await apiCall('/status');
    if (data) {
        document.getElementById('system-info').innerHTML = `
            <strong>Firmware:</strong> ${data.firmware || 'v0.1.0'}<br>
            <strong>Hardware:</strong> ${data.hardware || 'ESP32-C3'}<br>
            <strong>Host Communication:</strong> ${data.transport || 'Unknown'} - ${data.host_connected ? 'Connected' : 'Disconnected'}<br>
            <strong>Free Memory:</strong> ${data.free_memory || 'Unknown'} bytes<br>
            <strong>Uptime:</strong> ${data.uptime || 'Unknown'} seconds
        `;
        showStatus('success', 'Connected to front panel');
    } else {
        showStatus('error', 'Failed to connect to front panel');
    }
}

async function refreshDiscs() {
    const data = await apiCall('/discs');
    if (data) {
        const listDiv = document.getElementById('disc-list');
        if (data.discs && data.discs.length > 0) {
            listDiv.innerHTML = data.discs.map((disc, index) =>
                `<div class="disc-item">
                    <span><strong>${disc.name}</strong> (${(disc.size / 1000000).toFixed(1)} MB, ${disc.tracks} tracks)</span>
                    <button onclick="selectDisc(${index})">📀 Select</button>
                </div>`
            ).join('');
        } else {
            listDiv.innerHTML = '<div>No discs available</div>';
        }

        // Update current disc status
        document.getElementById('current-disc-name').textContent = data.current_disc || 'No disc loaded';
    }
}

async function selectDisc(index) {
    const data = await apiCall('/select_disc', {
        method: 'POST',
        body: JSON.stringify({ disc_index: index })
    });
    if (data && data.success) {
        await refreshDiscs();
        showStatus('success', `Selected disc ${index}`);
    }
}

async function ejectDisc() {
    const data = await apiCall('/eject_disc', { method: 'POST' });
    if (data && data.success) {
        await refreshDiscs();
        showStatus('success', 'Disc ejected');
    }
}

async function loadWiFiStatus() {
    const data = await apiCall('/wifi/status');
    if (data) {
        document.getElementById('wifi-state').textContent = data.state || 'Unknown';
        if (data.ip_address) {
            document.getElementById('wifi-status').innerHTML = `WiFi Status: <strong>${data.state}</strong> (IP: ${data.ip_address})`;
        }
    }
}

async function scanWiFi() {
    showStatus('', 'Scanning WiFi networks...');
    const data = await apiCall('/wifi/scan');
    if (data && data.networks) {
        const listDiv = document.getElementById('wifi-list');
        if (data.networks.length > 0) {
            listDiv.innerHTML = data.networks.map(network =>
                `<div class="wifi-item">
                    <span><strong>${network.ssid}</strong> <span class="signal-strength">(${network.rssi} dBm, ${network.auth_mode})</span></span>
                    <button onclick="connectToWiFi('${network.ssid}', null, ${!network.has_password})">🔗 Connect</button>
                </div>`
            ).join('');
        } else {
            listDiv.innerHTML = '<div>No networks found</div>';
        }
        showStatus('success', `Found ${data.networks.length} networks`);
    }
}

async function connectToWiFi(ssid, password, isOpen = false) {
    if (!isOpen && !password) {
        password = prompt(`Enter password for ${ssid}:`);
        if (!password) return;
    }

    showStatus('', `Connecting to ${ssid}...`);
    const data = await apiCall('/wifi/connect', {
        method: 'POST',
        body: JSON.stringify({ ssid: ssid, password: password || '' })
    });

    if (data) {
        if (data.success) {
            showStatus('success', `Connected to ${ssid}`);
            setTimeout(loadWiFiStatus, 2000);
        } else {
            showStatus('error', `Failed to connect: ${data.error || 'Unknown error'}`);
        }
    }
}

// Firmware update variables
let updateInProgress = false;
let updateCheckTimer = null;

// Format version number for display
function formatVersion(version) {
    const major = (version >> 16) & 0xFF;
    const minor = (version >> 8) & 0xFF;
    const patch = version & 0xFF;
    return `v${major}.${minor}.${patch}`;
}

async function checkFirmwareUpdate() {
    showStatus('', 'Checking for firmware updates...');

    const data = await apiCall('/firmware/check');
    if (data) {
        // Update current version display
        if (data.current_version !== undefined) {
            document.getElementById('current-version').textContent = formatVersion(data.current_version);
        }

        // Update available version display
        if (data.update_available) {
            document.getElementById('available-version').textContent = formatVersion(data.available_version);
            document.getElementById('available-version').style.color = '#28a745';
            document.getElementById('update-btn').disabled = false;
            showStatus('success', 'Firmware update available!');
        } else {
            document.getElementById('available-version').textContent = 'Up to date';
            document.getElementById('available-version').style.color = '#666';
            document.getElementById('update-btn').disabled = true;
            showStatus('success', 'Firmware is up to date');
        }
    }
}

async function startFirmwareUpdate() {
    if (updateInProgress) {
        showStatus('error', 'Update already in progress');
        return;
    }

    if (!confirm('Are you sure you want to update the firmware? The device will restart during this process.')) {
        return;
    }

    updateInProgress = true;
    document.getElementById('update-btn').disabled = true;
    document.getElementById('update-progress').style.display = 'block';
    document.getElementById('update-status').style.display = 'block';
    document.getElementById('update-status').textContent = 'Starting firmware update...';

    showStatus('', 'Starting firmware update...');

    const data = await apiCall('/firmware/update', { method: 'POST' });
    if (data && data.success) {
        // Start polling for progress
        updateCheckTimer = setInterval(checkUpdateProgress, 2000);
    } else {
        updateInProgress = false;
        document.getElementById('update-btn').disabled = false;
        document.getElementById('update-progress').style.display = 'none';
        document.getElementById('update-status').style.display = 'none';
        showStatus('error', `Failed to start update: ${data?.error || 'Unknown error'}`);
    }
}

async function checkUpdateProgress() {
    const data = await apiCall('/firmware/status');
    if (data) {
        const progress = data.progress || 0;
        const state = data.state || 'unknown';

        // Update progress bar
        document.getElementById('update-progress-fill').style.width = `${progress}%`;

        // Update status text
        let statusText = '';
        switch (state) {
            case 'downloading':
                statusText = `Downloading firmware... ${progress}%`;
                break;
            case 'verifying':
                statusText = 'Verifying firmware...';
                break;
            case 'applying':
                statusText = 'Applying update...';
                break;
            case 'success':
                statusText = 'Update successful! Device will restart...';
                updateInProgress = false;
                clearInterval(updateCheckTimer);
                showStatus('success', 'Firmware update completed successfully!');
                setTimeout(() => {
                    showStatus('', 'Device restarting...');
                }, 2000);
                break;
            case 'error':
                statusText = `Update failed: ${data.error || 'Unknown error'}`;
                updateInProgress = false;
                clearInterval(updateCheckTimer);
                document.getElementById('update-btn').disabled = false;
                showStatus('error', statusText);
                break;
            default:
                statusText = 'Preparing update...';
        }

        document.getElementById('update-status').textContent = statusText;

        // If update completed or failed, stop polling
        if (state === 'success' || state === 'error') {
            clearInterval(updateCheckTimer);
            updateCheckTimer = null;
        }
    }
}

// File upload variables
let uploadXHR = null;
let uploadStartTime = null;

function formatBytes(bytes) {
    if (bytes === 0) return '0 Bytes';
    const k = 1024;
    const sizes = ['Bytes', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function formatTime(seconds) {
    if (seconds < 60) return `${Math.round(seconds)}s`;
    const minutes = Math.floor(seconds / 60);
    const secs = Math.round(seconds % 60);
    if (minutes < 60) return `${minutes}m ${secs}s`;
    const hours = Math.floor(minutes / 60);
    const mins = minutes % 60;
    return `${hours}h ${mins}m`;
}

async function uploadFile() {
    const fileInput = document.getElementById('file-input');
    const file = fileInput.files[0];

    if (!file) {
        showStatus('error', 'Please select a file to upload');
        return;
    }

    // Reset UI
    document.getElementById('upload-btn').disabled = true;
    document.getElementById('cancel-upload-btn').style.display = 'inline-block';
    document.getElementById('upload-info').style.display = 'block';
    document.getElementById('upload-status').style.display = 'none';

    // Set file info
    document.getElementById('upload-filename').textContent = file.name;
    document.getElementById('upload-filesize').textContent = formatBytes(file.size);

    uploadStartTime = Date.now();

    // Create FormData
    const formData = new FormData();
    formData.append('fileSize', file.size.toString());

    // Add SHA256 hash as binary blob
    if (file.hashBuffer) {
        const hashBlob = new Blob([file.hashBuffer], { type: 'application/octet-stream' });
        formData.append('fileHash', hashBlob, 'fileHash');
    }

    formData.append('fileData', file);

    // Create XMLHttpRequest for progress tracking
    uploadXHR = new XMLHttpRequest();

    // Track upload progress
    uploadXHR.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
            const percentComplete = (e.loaded / e.total) * 100;
            const elapsedTime = (Date.now() - uploadStartTime) / 1000; // seconds
            const uploadSpeed = e.loaded / elapsedTime; // bytes per second
            const remainingBytes = e.total - e.loaded;
            const eta = remainingBytes / uploadSpeed; // seconds

            // Update UI
            document.getElementById('upload-progress-fill').style.width = `${percentComplete}%`;
            document.getElementById('upload-percent').textContent = `${Math.round(percentComplete)}%`;
            document.getElementById('upload-speed').textContent = formatBytes(uploadSpeed) + '/s';
            document.getElementById('upload-eta').textContent = remainingBytes > 0 ? formatTime(eta) : 'Complete';
        }
    });

    // Handle completion
    uploadXHR.addEventListener('load', () => {
        if (uploadXHR.status === 200) {
            try {
                const response = JSON.parse(uploadXHR.responseText);
                if (response.success) {
                    document.getElementById('upload-status').className = 'status success';
                    document.getElementById('upload-status').textContent = `File uploaded successfully to ${response.path || '/uploads/' + file.name}`;
                    document.getElementById('upload-status').style.display = 'block';
                    showStatus('success', 'File upload completed!');
                } else {
                    throw new Error(response.error || 'Upload failed');
                }
            } catch (error) {
                document.getElementById('upload-status').className = 'status error';
                document.getElementById('upload-status').textContent = `Upload failed: ${error.message}`;
                document.getElementById('upload-status').style.display = 'block';
                showStatus('error', 'Upload failed');
            }
        } else {
            document.getElementById('upload-status').className = 'status error';
            document.getElementById('upload-status').textContent = `Upload failed: Server returned ${uploadXHR.status}`;
            document.getElementById('upload-status').style.display = 'block';
            showStatus('error', 'Upload failed');
        }
        resetUploadUI();
    });

    // Handle errors
    uploadXHR.addEventListener('error', () => {
        document.getElementById('upload-status').className = 'status error';
        document.getElementById('upload-status').textContent = 'Upload failed: Network error';
        document.getElementById('upload-status').style.display = 'block';
        showStatus('error', 'File upload failed');
        resetUploadUI();
    });

    // Handle abort
    uploadXHR.addEventListener('abort', () => {
        document.getElementById('upload-status').className = 'status';
        document.getElementById('upload-status').textContent = 'Upload cancelled';
        document.getElementById('upload-status').style.display = 'block';
        showStatus('', 'Upload cancelled');
        resetUploadUI();
    });

    // Start upload
    uploadXHR.open('POST', '/api/upload');
    uploadXHR.send(formData);
}

function cancelUpload() {
    if (uploadXHR) {
        uploadXHR.abort();
    }
}

function resetUploadUI() {
    document.getElementById('upload-btn').disabled = false;
    document.getElementById('cancel-upload-btn').style.display = 'none';
    document.getElementById('file-input').value = '';
    uploadXHR = null;
    uploadStartTime = null;
}

// Initialize the page
document.addEventListener('DOMContentLoaded', function() {
    loadSystemInfo();
    refreshDiscs();
    loadWiFiStatus();
    checkFirmwareUpdate();

    // Add file input change listener to set file size
    const fileInput = document.getElementById('file-input');
    const fileSizeInput = document.getElementById('file-size');

    fileInput.addEventListener('change', async function() {
        if (this.files.length > 0) {
            const file = this.files[0];
            fileSizeInput.value = file.size.toString();

            // Calculate SHA256 hash of the file
            console.log('Calculating SHA256 hash...');
            const arrayBuffer = await file.arrayBuffer();
            let hashBuffer;

            if (crypto && crypto.subtle) {
                // Use native Web Crypto API if available (HTTPS/localhost)
                hashBuffer = await crypto.subtle.digest('SHA-256', arrayBuffer);
            } else {
                // Use fallback implementation for HTTP contexts
                console.log('crypto.subtle not available, using fallback SHA256');
                hashBuffer = sha256(arrayBuffer);
            }

            // Store hash buffer in a data attribute (binary)
            file.hashBuffer = hashBuffer;

            // For console logging, show hex representation
            const hashArray = Array.from(new Uint8Array(hashBuffer));
            const hashHex = hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
            console.log('File selected:', file.name, 'Size:', file.size, 'bytes', 'SHA256:', hashHex);
        } else {
            fileSizeInput.value = '';
            const hashInput = document.getElementById('file-hash');
            if (hashInput) hashInput.value = '';
        }
    });

    // Refresh data every 10 seconds
    setInterval(() => {
        // loadWiFiStatus();
    }, 10000);
});
