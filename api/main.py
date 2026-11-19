import csv
import io
import os
import secrets
from datetime import datetime, timezone
from pathlib import Path

from fastapi import FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel, Field
from sqlalchemy import Column, DateTime, Float, Integer, String, create_engine, select
from sqlalchemy.orm import Session, declarative_base

# Configuration comes from environment variables (never hardcoded secrets)
DATABASE_URL = os.getenv("DATABASE_URL", "sqlite:///./data/readings.db")
API_KEY = os.getenv("API_KEY", "")
STATIC_DIR = Path(__file__).parent / "static"

if DATABASE_URL.startswith("sqlite"):
    os.makedirs("data", exist_ok=True)

engine = create_engine(DATABASE_URL, pool_pre_ping=True)
Base = declarative_base()


class Reading(Base):
    """Database table that stores each sensor reading."""
    __tablename__ = "readings"

    id = Column(Integer, primary_key=True)
    device_id = Column(String(50), nullable=False, index=True)
    temperature = Column(Float, nullable=False)
    humidity = Column(Float, nullable=False)
    created_at = Column(
        DateTime(timezone=True),
        default=lambda: datetime.now(timezone.utc),
        index=True,
    )


Base.metadata.create_all(engine)


class ReadingIn(BaseModel):
    """Data the ESP32 sends. Pydantic rejects anything out of range."""
    device_id: str = Field(min_length=1, max_length=50, pattern=r"^[A-Za-z0-9_-]+$")
    temperature: float = Field(ge=-40, le=85)
    humidity: float = Field(ge=0, le=100)


class ReadingOut(BaseModel):
    """Data the API returns (only the necessary fields)."""
    id: int
    device_id: str
    temperature: float
    humidity: float
    created_at: datetime

    model_config = {"from_attributes": True}


app = FastAPI(title="Lab Climate Monitor API", version="2.0.0")


@app.middleware("http")
async def add_security_headers(request: Request, call_next):
    """Add basic security headers to every response."""
    response = await call_next(request)
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "no-referrer"
    return response


def check_api_key(received_key: str | None) -> None:
    """Reject the request if the API key is missing or wrong."""
    if not API_KEY or not received_key or not secrets.compare_digest(received_key, API_KEY):
        raise HTTPException(status_code=401, detail="Invalid or missing API key")


@app.get("/", include_in_schema=False)
def dashboard():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/api/readings", response_model=ReadingOut, status_code=201)
def create_reading(data: ReadingIn, x_api_key: str | None = Header(default=None)):
    check_api_key(x_api_key)
    with Session(engine) as session:
        reading = Reading(**data.model_dump())
        session.add(reading)
        session.commit()
        session.refresh(reading)
        return ReadingOut.model_validate(reading)


@app.get("/api/readings", response_model=list[ReadingOut])
def list_readings(limit: int = Query(default=50, ge=1, le=500)):
    with Session(engine) as session:
        query = select(Reading).order_by(Reading.created_at.desc()).limit(limit)
        rows = session.scalars(query).all()
        return [ReadingOut.model_validate(row) for row in rows]


@app.get("/api/readings/latest", response_model=ReadingOut)
def latest_reading():
    with Session(engine) as session:
        query = select(Reading).order_by(Reading.created_at.desc()).limit(1)
        row = session.scalars(query).first()
        if row is None:
            raise HTTPException(status_code=404, detail="No readings yet")
        return ReadingOut.model_validate(row)


@app.get("/api/readings/export.csv")
def export_csv():
    """Download every reading as a CSV file, oldest first."""
    buffer = io.StringIO()
    writer = csv.writer(buffer)
    writer.writerow(["id", "device_id", "temperature_c", "humidity_pct", "created_at_utc"])

    with Session(engine) as session:
        query = select(Reading).order_by(Reading.created_at.asc())
        for row in session.scalars(query):
            writer.writerow([
                row.id,
                row.device_id,
                row.temperature,
                row.humidity,
                row.created_at.isoformat(),
            ])

    return Response(
        content=buffer.getvalue(),
        media_type="text/csv",
        headers={"Content-Disposition": 'attachment; filename="lab_readings.csv"'},
    )
