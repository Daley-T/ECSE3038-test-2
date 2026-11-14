import json, os, secrets
import paho.mqtt.publish as publish

from datetime import datetime, timezone
from typing import Literal
from dotenv import load_dotenv
from fastapi import FastAPI, Header, HTTPException, Query, Response
from pydantic import BaseModel, Field
from pymongo import DESCENDING, MongoClient
from pymongo.errors import DuplicateKeyError


load_dotenv()
client = MongoClient(os.getenv("MONGODB_URI"), tz_aware=True)
db = client["Test_2"]
fans = db["fans"]
readings = db["readings"]
settings = db["settings"]

fans.create_index("mac", unique=True)
fans.create_index("key", unique=True)
readings.create_index("mac", unique=False)
settings.create_index("mac", unique=True)

app = FastAPI()


class Fan(BaseModel):
    mac: str
    room: str


class Readings(BaseModel):
    mac: str
    humidity: float


class Settings(BaseModel):
    mode: Literal["auto", "on", "off"]
    max_humidity: float = Field(ge=0, le=100)


def get_owner(x_api_key: str | None):
    owner = fans.find_one({"key": x_api_key}) if x_api_key else None
    if owner is None:
        raise HTTPException(status_code=401, detail="Missing or unknown API key.")
    return owner

@app.post("/fans", status_code=201)
def create_device(fan: Fan):
    new_fan = fan.model_dump()
    new_fan["key"] = secrets.token_urlsafe(16)
    try:
        fans.insert_one(new_fan)
    except DuplicateKeyError:
        raise HTTPException(status_code=409, detail=f"Fan with MAC {fan.mac} already exists.")
    new_fan.pop("_id")
    return new_fan 


@app.get("/fans")
def get_devices():
    return list(fans.find({}, {"_id": 0, "key": 0}))



@app.get("/fans/{mac}")
def get_device(mac: str):
    fan = fans.find_one({"mac": mac}, {"_id": 0, "key": 0})
    if fan is None:
        raise HTTPException(status_code=404, detail="Fan not found.")
    return fan


@app.post("/readings", status_code=201)
def create_reading(reading: Readings, x_api_key: str | None = Header(None)):
    owner = get_owner(x_api_key)                                   # 401
    if fans.find_one({"mac": reading.mac}) is None:
        raise HTTPException(status_code=404, detail="Fan not found.")
    if owner["mac"] != reading.mac:
        raise HTTPException(status_code=403, detail="Key belongs to another fan.")
    new_reading = reading.model_dump()
    new_reading["timestamp"] = datetime.now(timezone.utc)
    readings.insert_one(new_reading)
    new_reading.pop("_id")
    return new_reading


@app.get("/readings")
def get_readings(mac: str | None = None, limit: int = Query(10, ge=1, le=100)):
    query = {"mac": mac} if mac else {}
    return list(
        readings.find(query, {"_id": 0}).sort("timestamp", DESCENDING).limit(limit)
    )


@app.put("/fans/{mac}/settings", status_code=200)
def update_settings(mac: str, body: Settings, response: Response):
    if fans.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="Fan not found.")
    data = body.model_dump()
    result = settings.update_one({"mac": mac}, {"$set": data}, upsert=True)
    response.status_code = 201 if result.upserted_id is not None else 200
    publish.single(
        f"ecse3038/{os.getenv('MQTT_NAME')}/fan/settings",
        payload=json.dumps(data),
        retain=True,
        hostname=os.getenv("MQTT_BROKER"),
        port=int(os.getenv("MQTT_PORT", "1883")),
    )
    return data


@app.delete("/fans/{mac}", status_code=204)
def delete_fan(mac: str, x_api_key: str | None = Header(None)):
    owner = get_owner(x_api_key)                                   # 401
    if fans.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="Fan not found.")
    if owner["mac"] != mac:
        raise HTTPException(status_code=403, detail="Key belongs to another fan.")
    fans.delete_one({"mac": mac})
    settings.delete_one({"mac": mac})
    return Response(status_code=204)
