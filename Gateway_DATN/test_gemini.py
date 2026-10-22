import asyncio
import websockets
import json

API_KEY = "AQ.Ab8RN6IdLF13NdQRBBf7Ge9dh9sFjg8VX3JxyUwLeFzdmUVELA"
URL = f"wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={API_KEY}"

async def test():
    try:
        async with websockets.connect(URL) as ws:
            print("Connected!")
            setup_msg = {
                "setup": {
                    "model": "models/gemini-3.8-live"
                }
            }
            await ws.send(json.dumps(setup_msg))
            print("Sent setup")
            response = await ws.recv()
            print("Received:", response)
    except Exception as e:
        print("Error:", e)

asyncio.run(test())
