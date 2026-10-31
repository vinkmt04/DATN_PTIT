import asyncio
import websockets
import json

API_KEY = "AQ.Ab8RN6IdLF13NdQRBBf7Ge9dh9sFjg8VX3JxyUwLeFzdmUVELA"
URL = f"wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={API_KEY}"

async def test():
    try:
        async with websockets.connect(URL) as ws:
            setup_msg = {"setup": {"model": "models/gemini-3.8-live", "generationConfig": {"responseModalities": ["AUDIO"]}}}
            await ws.send(json.dumps(setup_msg))
            resp = await ws.recv()
            print("Setup Resp:", resp)
            
            end_msg = {"clientContent": {"turnComplete": True}}
            await ws.send(json.dumps(end_msg))
            print("Sent turnComplete")
            
            while True:
                resp = await asyncio.wait_for(ws.recv(), timeout=2.0)
                print("Resp:", resp)
    except Exception as e:
        print("Error:", e)

asyncio.run(test())
