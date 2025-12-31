// SPDX-License-Identifier: GPL-2.0-only
//
//  Copyright (C) 2025  Ian Scott
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License (as published by the
//  Free Software Foundation) version 2, dated June 1991.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License along
//  with this program; if not, see <https://www.gnu.org/licenses/>.

let currentImage = null;
let currentPath = "/";

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

async function refreshImages() {
    const data = await apiCall('/images');
    if (data) {
        currentPath = data.current_path || "/";
        currentImage = data.current_image;

        // Update path display
        const pathDisplay = document.getElementById('current-path');
        if (pathDisplay) {
            pathDisplay.textContent = currentPath;
        }

        // Update current image status
        const imageNameEl = document.getElementById('current-image-name');
        if (imageNameEl) {
            if (data.current_image) {
                imageNameEl.textContent = `${data.current_image} (${data.image_index + 1} of ${data.total_images})`;
            } else {
                imageNameEl.textContent = 'No image loaded';
            }
        }

        // Update prev/next button states
        const prevBtn = document.getElementById('prev-image-btn');
        const nextBtn = document.getElementById('next-image-btn');
        if (prevBtn) prevBtn.disabled = !data.current_image || data.total_images <= 1;
        if (nextBtn) nextBtn.disabled = !data.current_image || data.total_images <= 1;

        // Render directory entries
        const listDiv = document.getElementById('entry-list');
        if (!listDiv) return;

        // Show "Go Up" option if not at root
        let entriesHtml = '';
        if (currentPath !== '/' && currentPath !== '') {
            entriesHtml += `<div class="entry-item directory-item">
                <span><strong>..</strong> (Parent Directory)</span>
                <button onclick="selectEntry(-1)">Open</button>
            </div>`;
        }

        if (data.entries && data.entries.length > 0) {
            entriesHtml += data.entries.map(entry => {
                if (entry.is_directory) {
                    return `<div class="entry-item directory-item">
                        <span><strong>${entry.name}/</strong></span>
                        <button onclick="selectEntry(${entry.index})">Open</button>
                    </div>`;
                } else {
                    return `<div class="entry-item file-item">
                        <span><strong>${entry.name}</strong></span>
                        <button onclick="selectEntry(${entry.index})">Load</button>
                    </div>`;
                }
            }).join('');
        }

        if (entriesHtml === '') {
            entriesHtml = '<div>No entries in this directory</div>';
        }

        listDiv.innerHTML = entriesHtml;
    }
}

async function selectEntry(index) {
    const data = await apiCall('/select_entry', {
        method: 'POST',
        body: JSON.stringify({ index: index })
    });
    if (data && data.success) {
        await refreshImages();
        if (index === -1) {
            showStatus('success', 'Navigated to parent directory');
        } else {
            showStatus('success', 'Entry selected');
        }
    }
}

async function ejectImage() {
    const data = await apiCall('/eject_image', { method: 'POST' });
    if (data && data.success) {
        await refreshImages();
        showStatus('success', 'Image ejected');
    }
}

async function prevImage() {
    const data = await apiCall('/prev_image', { method: 'POST' });
    if (data && data.success) {
        await refreshImages();
        showStatus('success', 'Loaded previous image');
    }
}

async function nextImage() {
    const data = await apiCall('/next_image', { method: 'POST' });
    if (data && data.success) {
        await refreshImages();
        showStatus('success', 'Loaded next image');
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
let panelUpdateInProgress = false;
let panelUpdateCheckTimer = null;
let mainboardUpdateInProgress = false;
let mainboardUpdateCheckTimer = null;

// Format version number for display
function formatVersion(version) {
    if (version === 0) return 'Unknown';
    const major = (version >> 16) & 0xFF;
    const minor = (version >> 8) & 0xFF;
    const patch = version & 0xFF;
    return `v${major}.${minor}.${patch}`;
}

async function checkAllFirmware() {
    showStatus('', 'Checking for firmware updates...');
    await Promise.all([checkPanelFirmware(), checkMainboardFirmware()]);
    showStatus('success', 'Firmware check complete');
}

async function checkPanelFirmware() {
    const data = await apiCall('/firmware/check');
    if (data) {
        if (data.current_version !== undefined) {
            document.getElementById('panel-current-version').textContent = formatVersion(data.current_version);
        }

        if (data.update_available) {
            document.getElementById('panel-available-version').textContent = formatVersion(data.available_version);
            document.getElementById('panel-available-version').style.color = '#28a745';
            document.getElementById('panel-update-btn').disabled = false;
        } else {
            document.getElementById('panel-available-version').textContent = 'No update';
            document.getElementById('panel-available-version').style.color = '#666';
            document.getElementById('panel-update-btn').disabled = true;
        }
    }
}

async function checkMainboardFirmware() {
    const data = await apiCall('/firmware/mainboard/check');
    if (data) {
        if (data.current_version !== undefined) {
            document.getElementById('mainboard-current-version').textContent = formatVersion(data.current_version);
        }

        if (data.update_available) {
            document.getElementById('mainboard-available-version').textContent = formatVersion(data.available_version);
            document.getElementById('mainboard-available-version').style.color = '#28a745';
            document.getElementById('mainboard-update-btn').disabled = false;
        } else {
            document.getElementById('mainboard-available-version').textContent = 'No update';
            document.getElementById('mainboard-available-version').style.color = '#666';
            document.getElementById('mainboard-update-btn').disabled = true;
        }

        if (data.error) {
            document.getElementById('mainboard-available-version').textContent = 'Error';
            document.getElementById('mainboard-available-version').style.color = '#dc3545';
        }
    }
}

async function startPanelFirmwareUpdate() {
    if (panelUpdateInProgress) {
        showStatus('error', 'Panel update already in progress');
        return;
    }

    if (!confirm('Are you sure you want to update the front panel firmware? The panel will restart.')) {
        return;
    }

    panelUpdateInProgress = true;
    document.getElementById('panel-update-btn').disabled = true;
    document.getElementById('panel-update-progress').style.display = 'block';
    document.getElementById('panel-update-status').style.display = 'block';
    document.getElementById('panel-update-status').textContent = 'Starting panel update...';

    showStatus('', 'Starting panel firmware update...');

    const data = await apiCall('/firmware/update', { method: 'POST' });
    if (data && data.success) {
        panelUpdateCheckTimer = setInterval(checkPanelUpdateProgress, 2000);
    } else {
        panelUpdateInProgress = false;
        document.getElementById('panel-update-btn').disabled = false;
        document.getElementById('panel-update-progress').style.display = 'none';
        document.getElementById('panel-update-status').style.display = 'none';
        showStatus('error', `Failed to start panel update: ${data?.error || 'Unknown error'}`);
    }
}

async function checkPanelUpdateProgress() {
    const data = await apiCall('/firmware/status');
    if (data) {
        const progress = data.progress || 0;
        const state = data.state || 'unknown';

        document.getElementById('panel-update-progress-fill').style.width = `${progress}%`;

        let statusText = '';
        switch (state) {
            case 'downloading':
                statusText = `Downloading... ${progress}%`;
                break;
            case 'verifying':
                statusText = 'Verifying...';
                break;
            case 'applying':
                statusText = 'Applying update...';
                break;
            case 'success':
                statusText = 'Update successful! Panel restarting...';
                panelUpdateInProgress = false;
                clearInterval(panelUpdateCheckTimer);
                showStatus('success', 'Panel firmware update completed!');
                break;
            case 'error':
                statusText = `Failed: ${data.error || 'Unknown error'}`;
                panelUpdateInProgress = false;
                clearInterval(panelUpdateCheckTimer);
                document.getElementById('panel-update-btn').disabled = false;
                showStatus('error', statusText);
                break;
            default:
                statusText = 'Preparing...';
        }

        document.getElementById('panel-update-status').textContent = statusText;

        if (state === 'success' || state === 'error') {
            clearInterval(panelUpdateCheckTimer);
            panelUpdateCheckTimer = null;
        }
    }
}

async function startMainboardFirmwareUpdate() {
    if (mainboardUpdateInProgress) {
        showStatus('error', 'Main board update already in progress');
        return;
    }

    if (!confirm('Are you sure you want to update the main board firmware? The main board will restart.')) {
        return;
    }

    mainboardUpdateInProgress = true;
    document.getElementById('mainboard-update-btn').disabled = true;
    document.getElementById('mainboard-update-progress').style.display = 'block';
    document.getElementById('mainboard-update-status').style.display = 'block';
    document.getElementById('mainboard-update-status').textContent = 'Starting main board update...';

    showStatus('', 'Starting main board firmware update...');

    const data = await apiCall('/firmware/mainboard/update', { method: 'POST' });
    if (data && data.success) {
        // Main board update is fast - show fake progress for 5 seconds while it reboots
        await runMainboardUpdateAnimation();
    } else {
        mainboardUpdateInProgress = false;
        document.getElementById('mainboard-update-btn').disabled = false;
        document.getElementById('mainboard-update-progress').style.display = 'none';
        document.getElementById('mainboard-update-status').style.display = 'none';
        showStatus('error', `Failed to start main board update: ${data?.error || 'Unknown error'}`);
    }
}

async function runMainboardUpdateAnimation() {
    // Show progress animation for 5 seconds
    for (let progress = 0; progress <= 100; progress += 2) {
        document.getElementById('mainboard-update-progress-fill').style.width = `${progress}%`;
        document.getElementById('mainboard-update-status').textContent = `Updating main board... ${progress}%`;
        await new Promise(resolve => setTimeout(resolve, 100));
    }

    // Now wait for the board to come back online
    document.getElementById('mainboard-update-status').textContent = 'Waiting for main board to reboot...';

    // Try to get firmware status for up to 10 seconds
    let attempts = 0;
    while (attempts < 20) {
        await new Promise(resolve => setTimeout(resolve, 500));
        attempts++;

        try {
            const data = await apiCall('/firmware/mainboard/check');
            if (data && data.current_version) {
                // Board is back online!
                document.getElementById('mainboard-update-status').textContent =
                    `Update complete! Now running ${formatVersion(data.current_version)}`;
                document.getElementById('mainboard-current-version').textContent = formatVersion(data.current_version);
                showStatus('success', 'Main board firmware update completed!');
                mainboardUpdateInProgress = false;
                return;
            }
        } catch (e) {
            // Board still rebooting, continue waiting
        }
    }

    // Timed out waiting for board
    document.getElementById('mainboard-update-status').textContent = 'Update sent. Board may still be rebooting...';
    showStatus('', 'Update sent - board may still be rebooting');
    mainboardUpdateInProgress = false;
    document.getElementById('mainboard-update-btn').disabled = false;
}

// File upload variables
let uploadXHR = null;
let uploadStartTime = null;
let sha256Context = null;
let calculatedHash = null;

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

// Calculate SHA256 incrementally using requestIdleCallback
async function calculateSHA256Incrementally(file) {
    const chunkSize = 256 * 1024; // 256KB chunks
    const chunks = Math.ceil(file.size / chunkSize);
    let processedChunks = 0;

    // Initialize SHA256 context
    sha256Context = new SHA256Context();

    return new Promise((resolve, reject) => {
        function processNextChunk() {
            if (processedChunks >= chunks) {
                // All chunks processed, finalize hash
                console.log('Finalizing SHA256 calculation...');
                const hashBuffer = sha256Context.finalize();
                const hashArray = Array.from(new Uint8Array(hashBuffer));
                const hashHex = hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
                console.log('SHA256 calculated:', hashHex);
                resolve(hashBuffer);
                return;
            }

            // Process next chunk
            const start = processedChunks * chunkSize;
            const end = Math.min(start + chunkSize, file.size);
            const blob = file.slice(start, end);

            blob.arrayBuffer().then(buffer => {
                const data = new Uint8Array(buffer);
                sha256Context.update(data);
                processedChunks++;

                // Schedule next chunk processing
                if (window.requestIdleCallback) {
                    requestIdleCallback(() => processNextChunk(), { timeout: 50 });
                } else {
                    setTimeout(processNextChunk, 10);
                }
            }).catch(reject);
        }

        // Start processing
        processNextChunk();
    });
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

    // Start SHA256 calculation in background
    calculatedHash = null;
    const hashPromise = calculateSHA256Incrementally(file);

    // Create FormData
    const formData = new FormData();
    formData.append('fileSize', file.size.toString());
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
    uploadXHR.addEventListener('load', async () => {
        if (uploadXHR.status === 200) {
            try {
                const response = JSON.parse(uploadXHR.responseText);
                if (response.success) {
                    // Wait for SHA256 calculation to complete
                    document.getElementById('upload-status').className = 'status';
                    document.getElementById('upload-status').textContent = 'Verifying file integrity...';
                    document.getElementById('upload-status').style.display = 'block';

                    try {
                        calculatedHash = await hashPromise;

                        // Compare hash with server response
                        if (response.hash) {
                            const hashArray = Array.from(new Uint8Array(calculatedHash));
                            const hashHex = hashArray.map(b => b.toString(16).padStart(2, '0')).join('');

                            if (response.hash.toLowerCase() === hashHex.toLowerCase()) {
                                document.getElementById('upload-status').className = 'status success';
                                document.getElementById('upload-status').textContent = `File uploaded and verified successfully to ${response.path || '/uploads/' + file.name}`;
                                showStatus('success', 'File upload verified!');
                            } else {
                                document.getElementById('upload-status').className = 'status error';
                                document.getElementById('upload-status').textContent = 'Hash verification failed - file may be corrupted';
                                showStatus('error', 'Hash verification failed');
                            }
                        } else {
                            // Server should always return a hash
                            document.getElementById('upload-status').className = 'status error';
                            document.getElementById('upload-status').textContent = 'Server did not return file hash for verification';
                            showStatus('error', 'Missing hash from server');
                        }
                    } catch (hashError) {
                        console.error('SHA256 calculation failed:', hashError);
                        document.getElementById('upload-status').className = 'status error';
                        document.getElementById('upload-status').textContent = 'Failed to calculate file hash for verification';
                        showStatus('error', 'Hash calculation failed');
                    }
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
    calculatedHash = null;
    sha256Context = null;
}

// Initialize the page
document.addEventListener('DOMContentLoaded', function() {
    loadSystemInfo();
    refreshImages();
    loadWiFiStatus();
    checkAllFirmware();

    // Add file input change listener to set file size
    const fileInput = document.getElementById('file-input');
    const fileSizeInput = document.getElementById('file-size');

    fileInput.addEventListener('change', function() {
        if (this.files.length > 0) {
            const file = this.files[0];
            fileSizeInput.value = file.size.toString();
            console.log('File selected:', file.name, 'Size:', file.size, 'bytes');
            // SHA256 will be calculated during upload
            calculatedHash = null;
            sha256Context = null;
        } else {
            fileSizeInput.value = '';
            calculatedHash = null;
            sha256Context = null;
        }
    });

    // Refresh data every 10 seconds
    setInterval(() => {
        // loadWiFiStatus();
    }, 10000);
});
