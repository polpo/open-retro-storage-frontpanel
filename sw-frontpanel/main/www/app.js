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
let productName = "PicoIDE";
let mainboardFirmwarePending = false;
let devices = [];
let activeDeviceIndex = 0;
let deviceIndexInitialized = false;

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

let statusHideTimer = null;
const STATUS_HIDE_DELAY = 4000; // Hide after 4 seconds

function showStatus(type, message) {
    const statusDiv = document.getElementById('connection-status');
    statusDiv.className = `status status-bar ${type}`;
    statusDiv.textContent = message;
    statusDiv.classList.remove('hidden');

    // Clear any existing timer
    if (statusHideTimer) {
        clearTimeout(statusHideTimer);
        statusHideTimer = null;
    }

    // Only auto-hide non-error messages
    if (type !== 'error') {
        statusHideTimer = setTimeout(() => {
            statusDiv.classList.add('hidden');
        }, STATUS_HIDE_DELAY);
    }
}

async function loadSystemInfo() {
    const data = await apiCall('/status');
    if (data) {
        if (data.product_name) {
            productName = data.product_name;
        }
        if (data.product_full) {
            document.title = data.product_full;
        }
        // Update header logo/branding
        const heading = document.getElementById('product-heading');
        if (heading) {
            if (data.logo_url && data.logo_url.length > 0) {
                heading.innerHTML = '<img src="' + data.logo_url + '" alt="' + (data.product_name || 'Front Panel') + '" style="height: 48px;">';
            } else {
                heading.textContent = data.product_name || 'Front Panel';
            }
            heading.style.visibility = 'visible';
        }
        // Update hostname display
        const hostnameText = document.getElementById('hostname-text');
        const hostnameUrl = document.getElementById('hostname-url');
        if (hostnameText && hostnameUrl && data.hostname) {
            hostnameUrl.textContent = 'http://' + data.hostname + '.local';
            hostnameText.style.visibility = 'visible';
        }
        const freeMemKB = data.free_memory ? (data.free_memory / 1024).toFixed(1) + ' KB' : 'Unknown';
        const uptimeSec = data.uptime || 0;
        const uptimeMin = Math.floor(uptimeSec / 60);
        const uptimeRemSec = uptimeSec % 60;
        const uptimeStr = uptimeSec ? `${uptimeMin}:${String(uptimeRemSec).padStart(2, '0')}` : 'Unknown';
        document.getElementById('system-info').innerHTML = `
            <strong>Product:</strong> ${data.product_full || data.product_name || 'PicoIDE'}<br>
            <strong>Firmware:</strong> ${data.firmware || 'v0.1.0'}<br>
            <strong>Hardware:</strong> ${data.hardware || 'ESP32-C3'}<br>
            <strong>Host Communication:</strong> ${data.transport || 'Unknown'} - ${data.host_connected ? 'Connected' : 'Disconnected'}<br>
            <strong>Free Memory:</strong> ${freeMemKB}<br>
            <strong>Uptime:</strong> ${uptimeStr}
        `;
        setupMainboardFirmwareUI(productName);
        showStatus('success', 'Connected to front panel');
    } else {
        showStatus('error', 'Failed to connect to front panel');
    }
}

async function refreshDevices() {
    const data = await apiCall('/devices');
    if (data) {
        devices = data.devices || [];
        if (!deviceIndexInitialized && data.active_device !== undefined) {
            activeDeviceIndex = data.active_device;
            deviceIndexInitialized = true;
        }
        renderDeviceSelector();
    }
}

// S2S device types that support eject
const EJECTABLE_TYPES = [1, 2, 3, 4, 7]; // removable, optical, floppy, MO, ZIP

function renderDeviceSelector() {
    const container = document.getElementById('device-selector');
    if (!container) return;

    if (devices.length === 0) {
        container.innerHTML = '<div class="status">No devices found</div>';
        return;
    }

    container.innerHTML = '<div class="device-tabs">' +
        devices.map(dev => {
            const isActive = dev.index === activeDeviceIndex;
            const imageLine = dev.image ? dev.image : '[empty]';
            const canEject = dev.image && EJECTABLE_TYPES.includes(dev.type);
            const ejectBtn = canEject
                ? ` <button onclick="event.stopPropagation(); ejectDevice(${dev.index})" style="padding: 2px 8px; font-size: 11px; margin: 0;">Eject</button>`
                : '';
            return `<div class="device-tab${isActive ? ' active' : ''}" onclick="selectDevice(${dev.index})">` +
                `<strong>${dev.label}</strong>` +
                `<span class="device-image">${imageLine}${ejectBtn}</span>` +
                `</div>`;
        }).join('') +
        '</div>';
}

async function selectDevice(index) {
    activeDeviceIndex = index;
    renderDeviceSelector();
    await refreshImages();
}

async function refreshImages() {
    const endpoint = devices.length > 1 ? `/images?device=${activeDeviceIndex}` : '/images';
    const data = await apiCall(endpoint);
    if (data) {
        currentPath = data.current_path || "/";
        currentImage = data.current_image;

        // Update path display
        const pathDisplay = document.getElementById('current-path');
        if (pathDisplay) {
            pathDisplay.textContent = currentPath;
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

        const entries = (data.entries || []).filter(e => e.name !== '..' && e.name !== '.');
        if (entries.length > 0) {
            entriesHtml += entries.map(entry => {
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
    const body = { index: index };
    if (devices.length > 1) body.device = activeDeviceIndex;
    const data = await apiCall('/select_entry', {
        method: 'POST',
        body: JSON.stringify(body)
    });
    if (data && data.success) {
        await refreshImages();
        await refreshDevices();
        if (index === -1) {
            showStatus('success', 'Navigated to parent directory');
        } else {
            showStatus('success', 'Entry selected');
        }
    }
}

async function ejectDevice(deviceIndex) {
    const data = await apiCall('/eject_image', {
        method: 'POST',
        body: JSON.stringify({ device: deviceIndex })
    });
    if (data && data.success) {
        await refreshImages();
        await refreshDevices();
        showStatus('success', 'Image ejected');
    }
}

async function ejectImage() {
    const body = {};
    if (devices.length > 1) body.device = activeDeviceIndex;
    const data = await apiCall('/eject_image', {
        method: 'POST',
        body: JSON.stringify(body)
    });
    if (data && data.success) {
        await refreshImages();
        await refreshDevices();
        showStatus('success', 'Image ejected');
    }
}

async function prevImage() {
    const body = {};
    if (devices.length > 1) body.device = activeDeviceIndex;
    const data = await apiCall('/prev_image', {
        method: 'POST',
        body: JSON.stringify(body)
    });
    if (data && data.success) {
        await refreshImages();
        await refreshDevices();
        showStatus('success', 'Loaded previous image');
    }
}

async function nextImage() {
    const body = {};
    if (devices.length > 1) body.device = activeDeviceIndex;
    const data = await apiCall('/next_image', {
        method: 'POST',
        body: JSON.stringify(body)
    });
    if (data && data.success) {
        await refreshImages();
        await refreshDevices();
        showStatus('success', 'Loaded next image');
    }
}

let wifiSectionCollapsed = false;

function toggleWiFiSection() {
    const body = document.getElementById('wifi-config-body');
    if (!body) return;
    wifiSectionCollapsed = !wifiSectionCollapsed;
    body.style.display = wifiSectionCollapsed ? 'none' : '';
    const heading = document.getElementById('wifi-heading');
    if (heading) {
        heading.textContent = wifiSectionCollapsed ? '📶 WiFi Configuration ▸' : '📶 WiFi Configuration ▾';
    }
}

async function loadWiFiStatus() {
    const data = await apiCall('/wifi/status');
    if (data) {
        let statusText = data.state || 'Unknown';
        let wifiConfigured = false;
        if (data.mode === 'Client' && data.ssid) {
            statusText = `Connected to <strong>${data.ssid}</strong>`;
            if (data.ip_address) {
                statusText += ` (IP: ${data.ip_address})`;
            }
            wifiConfigured = true;
        } else if (data.mode === 'AP' && data.ssid) {
            statusText = `AP Mode: <strong>${data.ssid}</strong>`;
            if (data.ip_address) {
                statusText += ` (IP: ${data.ip_address})`;
            }
        }
        document.getElementById('wifi-state').textContent = data.state || 'Unknown';
        document.getElementById('wifi-status').innerHTML = `WiFi Status: ${statusText}`;

        // Collapse WiFi config section if already connected
        if (wifiConfigured && !wifiSectionCollapsed) {
            toggleWiFiSection();
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
function formatVersion(version, isDateBased) {
    if (version === 0) return 'Unknown';
    const major = (version >> 16) & 0xFF;
    const minor = (version >> 8) & 0xFF;
    const patch = version & 0xFF;
    if (isDateBased) {
        return `v${2000 + major}.${String(minor).padStart(2, '0')}.${String(patch).padStart(2, '0')}`;
    }
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
        } else if (data.available_version !== undefined) {
            // Firmware file exists but same version
            document.getElementById('panel-available-version').textContent = formatVersion(data.available_version);
            document.getElementById('panel-available-version').style.color = '#666';
            document.getElementById('panel-update-btn').disabled = true;
        } else {
            document.getElementById('panel-available-version').textContent = 'No update';
            document.getElementById('panel-available-version').style.color = '#666';
            document.getElementById('panel-update-btn').disabled = true;
        }
    }
}

async function checkBlueSCSIGitHubRelease(currentVersion) {
    try {
        const response = await fetch('https://api.github.com/repos/BlueSCSI/BlueSCSI-v2/releases/latest');
        if (!response.ok) return;
        const release = await response.json();

        // Parse tag like "v2025.02.08" or "2025.02.08"
        const match = release.tag_name.match(/v?(\d{4})\.(\d{1,2})\.(\d{1,2})/);
        if (!match) return;

        const year = parseInt(match[1]);
        const month = parseInt(match[2]);
        const day = parseInt(match[3]);
        const githubDate = year * 10000 + month * 100 + day;

        // Convert current packed version to comparable integer
        const curYear = 2000 + ((currentVersion >> 16) & 0xFF);
        const curMonth = (currentVersion >> 8) & 0xFF;
        const curDay = currentVersion & 0xFF;
        const currentDate = curYear * 10000 + curMonth * 100 + curDay;

        const versionStr = `v${year}.${String(month).padStart(2, '0')}.${String(day).padStart(2, '0')}`;

        const availableEl = document.getElementById('mainboard-available-version');
        const availableSection = document.getElementById('mainboard-available-section');
        if (availableSection) availableSection.style.display = '';

        if (githubDate > currentDate) {
            availableEl.textContent = versionStr;
            availableEl.style.color = '#28a745';
        } else {
            availableEl.textContent = versionStr + ' (up to date)';
            availableEl.style.color = '#666';
        }
    } catch (e) {
        console.log('GitHub release check failed:', e);
    }
}

async function checkMainboardFirmware() {
    const data = await apiCall('/firmware/mainboard/check');
    if (data) {
        const isDateVer = productName === 'BlueSCSI';
        if (data.current_version !== undefined) {
            document.getElementById('mainboard-current-version').textContent = formatVersion(data.current_version, isDateVer);
        }

        if (productName === 'BlueSCSI') {
            if (data.current_version !== undefined) {
                await checkBlueSCSIGitHubRelease(data.current_version);
            }
            return;
        }

        if (data.update_available) {
            document.getElementById('mainboard-available-version').textContent = formatVersion(data.available_version, isDateVer);
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
                panelUpdateInProgress = false;
                clearInterval(panelUpdateCheckTimer);
                showStatus('success', 'Panel firmware update completed!');
                startRebootCountdown();
                return;
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

function startRebootCountdown() {
    const statusEl = document.getElementById('panel-update-status');
    let remaining = 5;
    statusEl.textContent = `Update successful! Panel restarting... ${remaining}`;
    const timer = setInterval(() => {
        remaining--;
        if (remaining <= 0) {
            clearInterval(timer);
            location.reload();
        } else {
            statusEl.textContent = `Update successful! Panel restarting... ${remaining}`;
        }
    }, 1000);
}

async function startMainboardFirmwareUpdate() {
    if (mainboardUpdateInProgress) {
        showStatus('error', 'Main board update already in progress');
        return;
    }

    const confirmMsg = productName === 'BlueSCSI'
        ? 'Are you sure you want to reboot the main board to apply the uploaded firmware?'
        : 'Are you sure you want to update the main board firmware? The main board will restart.';
    if (!confirm(confirmMsg)) {
        return;
    }

    mainboardUpdateInProgress = true;
    document.getElementById('mainboard-update-btn').disabled = true;
    document.getElementById('mainboard-update-progress').style.display = 'block';
    document.getElementById('mainboard-update-status').style.display = 'block';

    const statusMsg = productName === 'BlueSCSI'
        ? 'Rebooting main board to apply firmware...'
        : 'Starting main board update...';
    document.getElementById('mainboard-update-status').textContent = statusMsg;

    showStatus('', statusMsg);

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
                const isDateVer = productName === 'BlueSCSI';
                document.getElementById('mainboard-update-status').textContent =
                    `Update complete! Now running ${formatVersion(data.current_version, isDateVer)}`;
                document.getElementById('mainboard-current-version').textContent = formatVersion(data.current_version, isDateVer);
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

    // Set file info
    document.getElementById('upload-filename').textContent = file.name;
    document.getElementById('upload-filesize').textContent = formatBytes(file.size);

    uploadStartTime = Date.now();

    // Start SHA256 calculation in background
    calculatedHash = null;
    const hashPromise = calculateSHA256Incrementally(file);

    // Create FormData (use current browse path as upload destination)
    const uploadPath = currentPath.endsWith('/') ? currentPath + file.name : currentPath + '/' + file.name;
    const formData = new FormData();
    formData.append('fileSize', file.size.toString());
    formData.append('fileData', file, uploadPath);

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
                    showStatus('', 'Verifying file integrity...');

                    try {
                        calculatedHash = await hashPromise;

                        // Compare hash with server response
                        if (response.hash) {
                            const hashArray = Array.from(new Uint8Array(calculatedHash));
                            const hashHex = hashArray.map(b => b.toString(16).padStart(2, '0')).join('');

                            if (response.hash.toLowerCase() === hashHex.toLowerCase()) {
                                showStatus('success', 'File uploaded and verified');
                            } else {
                                showStatus('error', 'Hash verification failed - file may be corrupted');
                            }
                        } else {
                            showStatus('error', 'Server did not return hash for verification');
                        }
                    } catch (hashError) {
                        console.error('SHA256 calculation failed:', hashError);
                        showStatus('error', 'Hash calculation failed');
                    }
                } else {
                    throw new Error(response.error || 'Upload failed');
                }
            } catch (error) {
                showStatus('error', `Upload failed: ${error.message}`);
            }
        } else {
            showStatus('error', `Upload failed: Server returned ${uploadXHR.status}`);
        }
        resetUploadUI();
    });

    // Handle errors
    uploadXHR.addEventListener('error', () => {
        showStatus('error', 'Upload failed: Network error');
        resetUploadUI();
    });

    // Handle abort
    uploadXHR.addEventListener('abort', () => {
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

function getConfigFilename() {
    return productName.toLowerCase() + '.ini';
}

// Config editor functions
async function loadConfig() {
    const editor = document.getElementById('config-editor');
    const configFile = getConfigFilename();

    showStatus('', 'Loading configuration...');

    // Update UI to reflect current product config filename
    const configHeading = document.getElementById('config-heading');
    if (configHeading) configHeading.textContent = configFile + ' Editor';
    editor.placeholder = 'Loading ' + configFile + '...';

    try {
        const response = await fetch('/api/download?path=/' + configFile);
        if (response.status === 404) {
            editor.value = '';
            editor.placeholder = 'No ' + configFile + ' found — enter configuration below and save to create it.';
            return;
        }
        if (!response.ok) {
            throw new Error(`Failed to load config: ${response.status}`);
        }
        const content = await response.text();
        editor.value = content;
        showStatus('success', 'Configuration loaded');
    } catch (error) {
        showStatus('error', `Config error: ${error.message}`);
    }
}

async function saveConfig() {
    const editor = document.getElementById('config-editor');
    const content = editor.value;

    if (!content.trim()) {
        showStatus('error', 'Configuration is empty');
        return;
    }

    showStatus('', 'Saving configuration...');

    try {
        // Create a Blob from the content
        const blob = new Blob([content], { type: 'text/plain' });

        // Create FormData with the file (use fileData to match server expectations)
        // Filename starts with '/' to indicate full path (not relative to /uploads/)
        const formData = new FormData();
        formData.append('fileSize', blob.size.toString());
        formData.append('fileData', blob, '/' + getConfigFilename());

        const response = await fetch('/api/upload', {
            method: 'POST',
            body: formData
        });

        if (!response.ok) {
            throw new Error(`Failed to save config: ${response.status}`);
        }

        const result = await response.json();
        if (result.success) {
            showStatus('success', 'Config saved. Reboot main board to apply.');
        } else {
            throw new Error(result.error || 'Unknown error');
        }
    } catch (error) {
        showStatus('error', `Save error: ${error.message}`);
    }
}

// Configure main board firmware UI based on product type
function setupMainboardFirmwareUI(product) {
    const mainboardType = document.getElementById('mainboard-type');
    const availableSection = document.getElementById('mainboard-available-section');
    const uploadSection = document.getElementById('mainboard-upload-section');
    const updateBtn = document.getElementById('mainboard-update-btn');

    if (product === 'BlueSCSI') {
        if (mainboardType) mainboardType.textContent = 'BlueSCSI';
        if (availableSection) availableSection.style.display = 'none';
        if (uploadSection) uploadSection.style.display = 'block';
        if (updateBtn) updateBtn.textContent = 'Reboot to Apply Update';
    } else {
        if (mainboardType) mainboardType.textContent = 'RP2350';
        if (availableSection) availableSection.style.display = '';
        if (uploadSection) uploadSection.style.display = 'none';
        if (updateBtn) updateBtn.textContent = 'Update Main Board';
    }
}

// Upload firmware file to main board (BlueSCSI path)
async function uploadMainboardFirmware() {
    const fileInput = document.getElementById('mainboard-firmware-file');
    const file = fileInput.files[0];

    if (!file) {
        showStatus('error', 'Please select a firmware file');
        return;
    }

    // Validate .bin extension
    if (!file.name.toLowerCase().endsWith('.bin')) {
        showStatus('error', 'Please select a .bin firmware file');
        return;
    }

    const uploadBtn = document.getElementById('mainboard-upload-btn');
    const progressBar = document.getElementById('mainboard-upload-progress');
    const progressFill = document.getElementById('mainboard-upload-progress-fill');
    const statusDiv = document.getElementById('mainboard-upload-status');

    uploadBtn.disabled = true;
    progressBar.style.display = 'block';
    statusDiv.style.display = 'block';
    statusDiv.textContent = 'Uploading firmware...';
    progressFill.style.width = '0%';

    const formData = new FormData();
    formData.append('fileSize', file.size.toString());
    formData.append('fileData', file, file.name);

    const xhr = new XMLHttpRequest();

    xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
            const percent = (e.loaded / e.total) * 100;
            progressFill.style.width = `${percent}%`;
            statusDiv.textContent = `Uploading firmware... ${Math.round(percent)}%`;
        }
    });

    xhr.addEventListener('load', () => {
        if (xhr.status === 200) {
            try {
                const response = JSON.parse(xhr.responseText);
                if (response.success) {
                    statusDiv.textContent = 'Firmware uploaded successfully. Click "Reboot to Apply Update" to apply.';
                    mainboardFirmwarePending = true;
                    document.getElementById('mainboard-update-btn').disabled = false;
                    showStatus('success', 'Firmware uploaded successfully');
                } else {
                    statusDiv.textContent = `Upload failed: ${response.error || 'Unknown error'}`;
                    showStatus('error', `Upload failed: ${response.error || 'Unknown error'}`);
                }
            } catch (e) {
                statusDiv.textContent = 'Upload failed: Invalid response';
                showStatus('error', 'Upload failed: Invalid response');
            }
        } else {
            statusDiv.textContent = `Upload failed: Server returned ${xhr.status}`;
            showStatus('error', `Upload failed: Server returned ${xhr.status}`);
        }
        uploadBtn.disabled = false;
    });

    xhr.addEventListener('error', () => {
        statusDiv.textContent = 'Upload failed: Network error';
        showStatus('error', 'Upload failed: Network error');
        uploadBtn.disabled = false;
    });

    xhr.open('POST', '/api/firmware/mainboard/upload');
    xhr.send(formData);
}

// Initialize the page (serialize requests to avoid overwhelming ESP32)
document.addEventListener('DOMContentLoaded', async function() {
    await loadSystemInfo();
    await refreshDevices();
    await refreshImages();
    await loadWiFiStatus();
    await checkAllFirmware();
    await loadConfig();

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
