#!/usr/bin/env python3
"""
reTerminal E1001 用 テキスト→フレーム HTTP サーバー

機能:
  - GET /         : テキスト投稿フォームとプレビュー画像を返す HTML ページ
  - GET /frame.bin: 現在のテキストから生成した 4階調フレーム (96,000 バイト) を返す
  - GET /preview.png: PNG プレビュー画像を返す
  - GET /text     : 現在のテキストを返す
  - GET /health   : ヘルスチェックエンドポイント
  - POST /set     : 表示テキストを更新する
使い方:
  python server/server.py --host 0.0.0.0 --port 8000
"""

from __future__ import annotations

import argparse
import datetime as dt
import html
import io
import os
import socket
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable
from urllib.parse import parse_qs, urlparse

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:  # pragma: no cover - exercised by runtime users
    raise SystemExit(
        "Pillow is required. Install it with: python -m pip install -r server/requirements.txt"
    ) from exc


# ===== パネル解像度 =====
WIDTH = 800
HEIGHT = 480
FRAME_BYTES = WIDTH * HEIGHT // 4  # 1ピクセル 2ビット → 96,000 バイト

# ===== 内部状態 =====
DEFAULT_TEXT = "Hello E-Paper"
STATE = {
    "text": DEFAULT_TEXT,      # 現在表示中のテキスト
    "updated": dt.datetime.now(),  # 最終更新日時
}

# コマンドライン --font で指定されたフォントパス (未指定時 None)
FONT_PATH: str | None = None

# システムフォントの候補一覧 (上から順に探索し最初に見つかったものを使用)
FONT_CANDIDATES = (
    # Windows 日本語フォント
    r"C:\Windows\Fonts\meiryo.ttc",
    r"C:\Windows\Fonts\YuGothM.ttc",
    r"C:\Windows\Fonts\YuGothR.ttc",
    r"C:\Windows\Fonts\msgothic.ttc",
    # Linux CJK フォント
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    # macOS フォールバック
    "/System/Library/Fonts/Helvetica.ttc",
)


def find_font() -> str | None:
    """フォントファイルを探索する。--font 指定があればそこを優先、なければ候補一覧を順に調べる。"""
    if FONT_PATH:
        path = Path(FONT_PATH)
        if path.exists():
            return str(path)
    for candidate in FONT_CANDIDATES:
        if Path(candidate).exists():
            return candidate
    return None


def load_font(size: int) -> ImageFont.ImageFont:
    """指定サイズのフォントを読み込む。見つからない場合は Pillow デフォルトフォントを使用する。"""
    font_path = find_font()
    if font_path:
        return ImageFont.truetype(font_path, size)
    return ImageFont.load_default()


def text_size(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont) -> tuple[int, int]:
    """テキストのピクセル幅・高さを返す (textbbox 利用)。"""
    bbox = draw.textbbox((0, 0), text or " ", font=font)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def wrap_paragraph(
    draw: ImageDraw.ImageDraw,
    paragraph: str,
    font: ImageFont.ImageFont,
    max_width: int,
) -> list[str]:
    """
    1段落のテキストを max_width 内に収まるように折り返す。
    CJK 文字対応のため、1文字ずつ追加して幅を確認する。
    """
    if not paragraph:
        return [""]

    lines: list[str] = []
    line = ""
    for char in paragraph:
        candidate = line + char
        width, _ = text_size(draw, candidate, font)
        if line and width > max_width:
            # 最大幅を超えたら改行
            lines.append(line.rstrip())
            line = char.lstrip()
        else:
            line = candidate
    if line:
        lines.append(line.rstrip())
    return lines


def wrap_text(
    draw: ImageDraw.ImageDraw,
    text: str,
    font: ImageFont.ImageFont,
    max_width: int,
) -> list[str]:
    """改行で分割した各段落を wrap_paragraph で折り返す。"""
    lines: list[str] = []
    for paragraph in text.splitlines() or [""]:
        lines.extend(wrap_paragraph(draw, paragraph, font, max_width))
    return lines


def line_height(draw: ImageDraw.ImageDraw, font: ImageFont.ImageFont) -> int:
    """フォントの行高を返す。最小値 12px 以上。"""
    _, height = text_size(draw, "Ag", font)
    return max(height, 12)


def choose_body_font(
    draw: ImageDraw.ImageDraw,
    text: str,
    max_width: int,
    max_height: int,
) -> tuple[ImageFont.ImageFont, list[str], int]:
    """
    テキストが表示エリアに収まる最大のフォントサイズを自動選択する。
    64pt から 2pt 刻みで小さくしながら試し、収まればそのサイズを使用する。
    返り値: (font, 折り返し済み行リスト, 行間距)
    """
    for size in range(64, 23, -2):
        font = load_font(size)
        lines = wrap_text(draw, text, font, max_width)
        leading = max(8, size // 5)  # 行間距はフォントサイズの 1/5 以上
        total_height = len(lines) * line_height(draw, font) + max(0, len(lines) - 1) * leading
        if total_height <= max_height:
            return font, lines, leading

    # 最小サイズ (24pt) でも入らない場合は最大 12 行かつ末尾に "..." を付ける
    font = load_font(24)
    lines = wrap_text(draw, text, font, max_width)
    leading = 6
    while len(lines) > 12:
        last = lines[:11]
        last.append(lines[11][: max(0, len(lines[11]) - 1)] + "...")
        lines = last
    return font, lines, leading


def draw_rules(draw: ImageDraw.ImageDraw) -> None:
    """
    ヘッダー・フッター・区切り線を描画する。
    上部黒バー・下部黒バー・本文上下のグレー参考線。
    """
    draw.rectangle((0, 0, WIDTH, 54), fill=0)               # 上部黒バー (タイトルエリア)
    draw.rectangle((0, HEIGHT - 36, WIDTH, HEIGHT), fill=0)  # 下部黒バー (フッターエリア)
    draw.line((36, 92, WIDTH - 36, 92), fill=170, width=2)            # 本文上区切り線
    draw.line((36, HEIGHT - 76, WIDTH - 36, HEIGHT - 76), fill=170, width=2)  # 本文下区切り線


def build_image(text: str) -> Image.Image:
    """
    テキストから 800x480 のグレースケール画像を生成する。
    ヘッダーにタイトルとタイムスタンプ、中央に本文、フッターにエンドポイント URL を表示する。
    """
    # L モード = グレースケール (0=黒, 255=白)
    image = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(image)
    draw_rules(draw)

    title_font = load_font(28)
    small_font = load_font(18)
    now = dt.datetime.now()

    # ヘッダー: 左側にタイトル、右側に日時を白文字で描画
    draw.text((34, 14), "Home E-Paper", fill=255, font=title_font)
    stamp = now.strftime("%Y-%m-%d %H:%M")
    stamp_width, _ = text_size(draw, stamp, small_font)
    draw.text((WIDTH - 34 - stamp_width, 20), stamp, fill=255, font=small_font)

    # 本文エリア: 自動フォントサイズ選択して中央配置で描画
    body_area = (52, 112, WIDTH - 52, HEIGHT - 98)
    body_font, lines, leading = choose_body_font(
        draw,
        text.strip() or DEFAULT_TEXT,
        body_area[2] - body_area[0],
        body_area[3] - body_area[1],
    )
    body_line_height = line_height(draw, body_font)
    total_height = len(lines) * body_line_height + max(0, len(lines) - 1) * leading
    # 垂直方向も中央揃え
    y = body_area[1] + max(0, (body_area[3] - body_area[1] - total_height) // 2)

    for line in lines:
        width, _ = text_size(draw, line, body_font)
        # 水平方向も中央揃え
        x = body_area[0] + max(0, (body_area[2] - body_area[0] - width) // 2)
        draw.text((x, y), line, fill=0, font=body_font)
        y += body_line_height + leading

    # フッター: デバイスが取得するエンドポイント URL を白文字で表示
    footer = "GET /frame.bin"
    draw.text((34, HEIGHT - 27), footer, fill=255, font=small_font)
    return image


def pack_gray4(image: Image.Image) -> bytes:
    """
    PIL 画像を reTerminal E1001 スケッチのフレームフォーマットに変換する。
    グレースケール値 (0–255) を 4階調 (0=黒、3=白) に陰間化し、
    1バイトに 4ピクセルを評 (bits 7..6, 5..4, 3..2, 1..0) に詳める。
    """
    gray = image.convert("L")
    pixels = gray.tobytes()
    out = bytearray(FRAME_BYTES)
    pos = 0
    for y in range(HEIGHT):
        row = y * WIDTH
        for x in range(0, WIDTH, 4):
            byte = 0
            for offset in range(4):
                value = pixels[row + x + offset]
                level = min(3, value // 64)  # 0–63→0, 64–127→1, 128–191→2, 192–255→3
                byte |= level << (6 - offset * 2)
            out[pos] = byte
            pos += 1
    return bytes(out)


def build_frame(text: str) -> bytes:
    """テキストから 4階調フレームバイナリを一括生成する。"""
    return pack_gray4(build_image(text))


def to_png(image: Image.Image) -> bytes:
    """画像を PNG バイナリにエンコードして返す。"""
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


def local_ip_hint() -> str:
    """起動ログ表示用にサーバーのローカル IP アドレスを取得する。失敗時は localhost を返す。"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))  # 実際に接続しないダミー接続
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


class Handler(BaseHTTPRequestHandler):
    """各リクエストを処理する HTTP リクエストハンドラ。"""

    server_version = "E1001TextServer/1.0"

    def do_GET(self) -> None:
        """ルーティング: パスに応じて各エンドポイントにディスパッチする。"""
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)
        if parsed.path in ("/", "/index.html"):
            self.send_index()                                          # 管理 UI HTML
        elif parsed.path == "/frame.bin":
            text = params.get("text", [STATE["text"]])[0]
            self.send_bytes(build_frame(text), "application/octet-stream")  # 4階調バイナリ
        elif parsed.path == "/preview.png":
            text = params.get("text", [STATE["text"]])[0]
            self.send_bytes(to_png(build_image(text)), "image/png")  # PNG プレビュー
        elif parsed.path == "/text":
            self.send_bytes(str(STATE["text"]).encode("utf-8"), "text/plain; charset=utf-8")  # 現在テキスト
        elif parsed.path == "/health":
            self.send_bytes(b"ok\n", "text/plain; charset=utf-8")    # ヘルスチェック
        else:
            self.send_error(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self) -> None:
        """
        POST /set: 表示テキストを更新する。
        フォームデータの 'text' パラメータを受け取り STATE に保存する。
        成功時は / にリダイレクトする (POST/Redirect/GET パターン)。
        """
        parsed = urlparse(self.path)
        if parsed.path != "/set":
            self.send_error(HTTPStatus.NOT_FOUND, "Not found")
            return

        length = int(self.headers.get("Content-Length", "0"))
        # 大きすぎるリクエストは拒否 (16 KB 以上)
        if length > 16384:
            self.send_error(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Text too large")
            return

        body = self.rfile.read(length).decode("utf-8", errors="replace")
        data = parse_qs(body)
        STATE["text"] = data.get("text", [DEFAULT_TEXT])[0]
        STATE["updated"] = dt.datetime.now()
        # PRG パターン: ブラウザの二重送信を防ぐため 303 See Other で / にリダイレクト
        self.send_response(HTTPStatus.SEE_OTHER)
        self.send_header("Location", "/")
        self.end_headers()

    def send_index(self) -> None:
        """管理 UI の HTML ページを生成して返す。"""
        text = str(STATE["text"])
        updated = STATE["updated"].strftime("%Y-%m-%d %H:%M:%S")
        page = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>E1001 Text Server</title>
  <style>
    body {{ font-family: system-ui, sans-serif; max-width: 920px; margin: 32px auto; padding: 0 18px; }}
    textarea {{ width: 100%; min-height: 150px; font-size: 18px; padding: 12px; box-sizing: border-box; }}
    button {{ margin-top: 12px; padding: 10px 16px; font-size: 16px; }}
    img {{ width: 100%; max-width: 800px; border: 1px solid #bbb; image-rendering: pixelated; }}
    code {{ background: #eee; padding: 2px 5px; }}
  </style>
</head>
<body>
  <h1>E1001 Text Server</h1>
  <form method="post" action="/set">
    <textarea name="text">{html.escape(text)}</textarea>
    <br>
    <button type="submit">Update text</button>
  </form>
  <p>Updated: {html.escape(updated)}</p>
  <p>Device endpoint: <code>/frame.bin</code> ({FRAME_BYTES} bytes)</p>
  <p><img src="/preview.png" alt="preview"></p>
</body>
</html>
"""
        self.send_bytes(page.encode("utf-8"), "text/html; charset=utf-8")

    def send_bytes(self, payload: bytes, content_type: str) -> None:
        """バイナリデータを適切なヘッダー付きで返す。Cache-Control: no-store でキャッシュを無効化する。"""
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt: str, *args: object) -> None:
        """リクエストログを日時スタンプ付きで stdout に出力する。"""
        print(f"[{dt.datetime.now():%Y-%m-%d %H:%M:%S}] {self.address_string()} {fmt % args}")


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    """コマンドライン引数を解析する。各項目は環境変数でも上書き可能。"""
    parser = argparse.ArgumentParser(description="Serve E1001 4-gray frames generated from text.")
    parser.add_argument("--host", default=os.environ.get("HOST", "0.0.0.0"))              # リッスン IP
    parser.add_argument("--port", type=int, default=int(os.environ.get("PORT", "8000")))  # ポート番号
    parser.add_argument("--text", default=os.environ.get("EPAPER_TEXT", DEFAULT_TEXT))    # 初期テキスト
    parser.add_argument("--font", default=os.environ.get("EPAPER_FONT"))                  # フォントパス
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> None:
    """サーバーのエントリーポイント。引数を解析して HTTP サーバーを起動する。"""
    global FONT_PATH

    args = parse_args(argv)
    FONT_PATH = args.font
    STATE["text"] = args.text
    STATE["updated"] = dt.datetime.now()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    hint = local_ip_hint()
    font = find_font() or "Pillow default font"
    print(f"Serving on http://{hint}:{args.port}/")
    print(f"Frame endpoint: http://{hint}:{args.port}/frame.bin")
    print(f"Font: {font}")
    server.serve_forever()  # Ctrl+C で停止


if __name__ == "__main__":
    main()
