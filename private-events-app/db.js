const Database = require('better-sqlite3');
const path = require('path');

const db = new Database(path.join(__dirname, 'events.db'));

// Schema
db.exec(`
  CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    name TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    owner_id INTEGER NOT NULL REFERENCES users(id),
    title TEXT NOT NULL,
    description TEXT,
    event_date DATE NOT NULL,
    event_time TEXT NOT NULL,
    location TEXT,
    invite_slug TEXT UNIQUE NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS rsvps (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id INTEGER NOT NULL REFERENCES events(id),
    user_id INTEGER NOT NULL REFERENCES users(id),
    qr_token TEXT UNIQUE NOT NULL,
    status TEXT DEFAULT 'confirmed' CHECK(status IN ('confirmed', 'checked_in')),
    checked_in_at DATETIME,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(event_id, user_id)
  );

  CREATE INDEX IF NOT EXISTS idx_rsvps_qr_token ON rsvps(qr_token);
  CREATE INDEX IF NOT EXISTS idx_events_invite_slug ON events(invite_slug);
`);

module.exports = db;
