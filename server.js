const express = require('express');
const sqlite3 = require('sqlite3').verbose();
const cors = require('cors');

const app = express();
app.use(cors());
app.use(express.json());

// Connect to your local SQLite database engine
const db = new sqlite3.Database('./database.db', (err) => {
    if (err) {
        console.error("Database connection fault:", err.message);
    } else {
        console.log("Connected to the SQLite database pipeline.");
        
        // Ensure the logs table structurally exists
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
    }
});

// POST: Endpoint where your ESP32 pushes live telemetry data
app.post('/api/sensor-data', (expressReq, expressRes) => {
    const { device_id, temperature, humidity, sound_level, air_quality } = expressReq.body;

    // Calculate the Status matching your progressive buzzer rules
    let calculatedStatus = "Normal";
    
    if (air_quality > 500) {
        calculatedStatus = "Critical";
    } else if (air_quality > 300) {
        calculatedStatus = "Warning";
    } else if (temperature > 30.0 || temperature < 20.0 || humidity < 40.0 || humidity > 70.0 || sound_level > 500) {
        calculatedStatus = "Warning";
    }

    // Capture standard ISO timestamp ending in 'Z' (UTC format)
    const timestamp = new Date().toISOString();

    const logSql = `INSERT INTO logs (device_id, temperature, humidity, sound_level, air_quality, status, created_at) 
                    VALUES (?, ?, ?, ?, ?, ?, ?)`;
    const logParams = [device_id, temperature, humidity, sound_level, air_quality, calculatedStatus, timestamp];

    db.run(logSql, logParams, function (err) {
        if (err) {
            console.error("Failed to insert telemetry register:", err.message);
            return expressRes.status(500).json({ error: err.message });
        }

        // Default setting fallbacks for incoming edge responses
        const systemSettings = {
            temp_threshold: 32.0,
            gas_threshold: 400,
            upload_interval: 10
        };

        expressRes.status(201).json({
            message: "Telemetry packet processed.",
            log_id: this.lastID,
            status: calculatedStatus,
            settings: {
                temp_threshold: systemSettings.temp_threshold,
                gas_threshold: systemSettings.gas_threshold,
                upload_interval: systemSettings.upload_interval
            }
        });
    });
});

// GET: Enhanced historical logs endpoint with dynamic time-range query filtering
app.get('/api/logs', (expressReq, expressRes) => {
    const range = expressReq.query.range; 
    let timeFilterSql = "";

    // Uses strftime to compare standard UTC ISO-8601 strings cleanly
    if (range === '1h') {
        timeFilterSql = "WHERE created_at >= strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-1 hour')";
    } else if (range === '24h') {
        timeFilterSql = "WHERE created_at >= strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-24 hours')";
    } else if (range === '7d') {
        timeFilterSql = "WHERE created_at >= strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-7 days')";
    }

    // Default pulls overall telemetry logs chronological stream so UI is never blank
    const sql = `SELECT * FROM logs ${timeFilterSql} ORDER BY created_at DESC LIMIT 100`;
    
    db.all(sql, [], (err, rows) => {
        if (err) {
            return expressRes.status(500).json({ error: err.message });
        }
        expressRes.json(rows); 
    });
});

const PORT = 3000;
app.listen(PORT, () => {
    console.log(`Server handling background streams running on port ${PORT}`);
});