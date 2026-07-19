const express = require('express');
const path = require('path');
const sqlite3 = require('sqlite3').verbose();
const cors = require('cors');

const app = express();
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'dashboard')));

app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'dashboard', 'index.html'));
});

const DB_PATH = './database.db';
const DEFAULT_DEVICE_ID = 'ESP32_ROOM_1';

const db = new sqlite3.Database(DB_PATH, (err) => {
    if (err) {
        console.error('Database connection fault:', err.message);
        return;
    }
    initializeDatabase();
});

function initializeDatabase() {
    db.serialize(() => {
        db.run(`CREATE TABLE IF NOT EXISTS logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT,
            temperature REAL,
            humidity REAL,
            sound_level INTEGER,
            air_quality INTEGER,
            status TEXT,
            created_at TEXT
        )`);

        db.run(`CREATE TABLE IF NOT EXISTS device_settings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT UNIQUE,
            temp_threshold REAL DEFAULT 30,
            humidity_min REAL DEFAULT 40,
            humidity_max REAL DEFAULT 70,
            noise_threshold INTEGER DEFAULT 500,
            gas_warning INTEGER DEFAULT 300,
            gas_critical INTEGER DEFAULT 500,
            upload_interval INTEGER DEFAULT 10,
            alert_mode TEXT DEFAULT 'AUTO'
        )`, () => {
            db.run(`INSERT OR IGNORE INTO device_settings (device_id) VALUES ('${DEFAULT_DEVICE_ID}')`);
        });
    });
}

// 1. POST: Sensor Telemetry Intake
app.post('/api/sensor-data', (req, res) => {
    const { device_id, temperature, humidity, sound_level, air_quality } = req.body;
    const targetId = device_id || DEFAULT_DEVICE_ID;
    const timestamp = new Date().toISOString();

    db.get('SELECT * FROM device_settings WHERE device_id = ?', [targetId], (err, settings) => {
        let active = settings || { temp_threshold: 30, noise_threshold: 500, gas_warning: 300, gas_critical: 500 };
        
        let status = 'Normal';
        if (Number(air_quality) > Number(active.gas_critical)) status = 'Critical';
        else if (Number(air_quality) > Number(active.gas_warning) || Number(temperature) > Number(active.temp_threshold) || Number(sound_level) > Number(active.noise_threshold)) {
            status = 'Warning';
        }

        db.run(`INSERT INTO logs (device_id, temperature, humidity, sound_level, air_quality, status, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)`,
            [targetId, temperature, humidity, sound_level, air_quality, status, timestamp], function(e) {
                if (e) return res.status(500).json({ error: e.message });
                res.json({ message: 'Success', evaluated_status: status });
            });
    });
});

// 2. GET: Settings Endpoint (FIXED: Supports both dashboard and C++ key naming conventions)
app.get('/api/settings', (req, res) => {
    const device_id = req.query.device_id || DEFAULT_DEVICE_ID;
    db.get('SELECT * FROM device_settings WHERE device_id = ?', [device_id], (err, row) => {
        if (err) return res.status(500).json({ error: err.message });
        if (!row) return res.json({});

        res.json({
            device_id: row.device_id,
            temp_threshold: Number(row.temp_threshold),
            humidity_min: Number(row.humidity_min),
            humidity_max: Number(row.humidity_max),
            noise_threshold: Number(row.noise_threshold),
            upload_interval: Number(row.upload_interval),
            alert_mode: row.alert_mode,
            // Cross-mapping properties to keep database schema and C++ variables perfectly aligned
            gas_warning: Number(row.gas_warning),
            gas_critical: Number(row.gas_critical),
            gas_warning_threshold: Number(row.gas_warning), 
            gas_critical_threshold: Number(row.gas_critical)
        });
    });
});

// 3. POST: Save Settings
app.post('/api/settings', (req, res) => {
    const { device_id, temp_threshold, humidity_min, humidity_max, noise_threshold, gas_warning, gas_critical, upload_interval, alert_mode } = req.body;
    const targetId = device_id || DEFAULT_DEVICE_ID;

    const query = `UPDATE device_settings SET 
        temp_threshold = ?, humidity_min = ?, humidity_max = ?, noise_threshold = ?, 
        gas_warning = ?, gas_critical = ?, upload_interval = ?, alert_mode = ? WHERE device_id = ?`;
    
    db.run(query, [temp_threshold, humidity_min, humidity_max, noise_threshold, gas_warning, gas_critical, upload_interval, alert_mode, targetId], (err) => {
        if (err) return res.status(500).json({ error: err.message });
        res.json({ status: 'Configuration Updated Successfully' });
    });
});

app.get('/api/logs', (req, res) => {
    db.all('SELECT * FROM logs ORDER BY id DESC LIMIT 100', [], (err, rows) => { res.json(rows || []); });
});

app.get('/api/latest', (req, res) => {
    db.get('SELECT * FROM logs ORDER BY id DESC LIMIT 1', [], (err, row) => { res.json(row || {}); });
});

app.get('/api/statistics', (req, res) => {
    db.get(`SELECT COUNT(*) AS total_logs, AVG(temperature) AS avg_temp, AVG(humidity) AS avg_humidity, AVG(sound_level) AS sound_level, MAX(sound_level) AS peak_noise, MAX(air_quality) AS air_quality FROM logs`, [], (err, row) => {
        res.json(row || {});
    });
});

app.listen(3000, () => console.log(`Backend Server Engine listening on port 3000`));