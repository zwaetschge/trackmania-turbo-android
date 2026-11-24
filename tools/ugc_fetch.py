#!/usr/bin/env python3
"""Fetch community tracks of Trackmania Turbo (PC) from Ubisoft's track portal.

    tools/ugc_fetch.py list  <out dir> [--all] [--pages N]   index of tracks -> <out>/index.json
    tools/ugc_fetch.py pack  <out dir> <file.tar> [--limit N]   tar for the device's ImportTracks folder
    tools/ugc_fetch.py fetch <out dir> [--limit N] [--env Canyon,Valley,Lagoon,Stadium] [--jobs 4] [--with-nadeo]
                                                             map files -> <out>/maps/<uid>.Map.Gbx
                                                             (most played first; Nadeo's own uploads -- the
                                                             campaign -- are left out unless --with-nadeo)

The portal (https://players.turbo.trackmania.com/pc/maps) lists the tracks players shared
from the Track Lab: 40 per page, by default those played at least 500 times (about 1 900),
with --all every track (about 58 000). The map files are public on the portal's S3 bucket
as <mapUid>.Map.Gbx, next to the thumbnails. Pages are fetched one per second; files with
a few connections at a time. Both steps resume: what is there is not fetched again.
"""
import html
import json
import os
import re
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

PORTAL = 'https://players.turbo.trackmania.com/pc/maps'
BUCKET = 'https://tmturbo-prod-pc-maps.s3.amazonaws.com/%s.Map.Gbx'
UA = 'tmturbo-android-port/1.0 (personal offline copy of shared tracks)'


def get(url, tries=4):
    for k in range(tries):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': UA})
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except Exception as e:                       # noqa: BLE001 - network: retry, then give up
            if k == tries - 1:
                raise
            time.sleep(2 + 3 * k)


CARD = re.compile(r'<a class="no_underline" href="/pc/maps/([A-Za-z0-9_]{20,})">(.*?)</a>\s*</div>', re.S)


def parse_cards(page):
    out = []
    for uid, body in CARD.findall(page):
        title = re.search(r'<h4 class="media-heading">(.*?)</h4>', body, re.S)
        author = re.search(r'<p>by (.*?)</p>', body, re.S)
        plays = re.search(r'fa-gamepad"></i>\s*([\d,.\s]+)</p>', body)
        env = re.search(r'alt="(\w+) logo"', body)
        stars = body.count('fa-star"') + 0.5 * body.count('fa-star-half-o"')
        out.append({
            'uid': uid,
            'title': html.unescape(title.group(1).strip()) if title else '',
            'author': html.unescape(author.group(1).strip()) if author else '',
            'plays': int(re.sub(r'\D', '', plays.group(1))) if plays and re.sub(r'\D', '', plays.group(1)) else 0,
            'env': env.group(1) if env else '',
            'stars': stars,
        })
    return out


def cmd_list(out, all_tracks, max_pages):
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, 'index.json')
    idx = json.load(open(path)) if os.path.exists(path) else {'pages': {}, 'all': all_tracks}
    q = 'all_map_filter%5BminPlayCount%5D=0&' if all_tracks else ''
    page = 1
    while page <= max_pages:
        key = str(page)
        if key not in idx['pages']:
            cards = parse_cards(get('%s?%spage=%d' % (PORTAL, q, page)).decode('utf-8', 'replace'))
            idx['pages'][key] = cards
            json.dump(idx, open(path + '.tmp', 'w'))
            os.replace(path + '.tmp', path)
            time.sleep(1.0)
        if not idx['pages'][key]:
            break
        if page % 10 == 0:
            print('page %d: %d tracks so far' % (page, sum(len(v) for v in idx['pages'].values())), flush=True)
        page += 1
    tracks = {t['uid']: t for v in idx['pages'].values() for t in v}
    print('%d tracks indexed in %s' % (len(tracks), path))


def cmd_fetch(out, limit, envs, jobs, with_nadeo=False):
    idx = json.load(open(os.path.join(out, 'index.json')))
    tracks = list({t['uid']: t for v in idx['pages'].values() for t in v}.values())
    if not with_nadeo:                              # Nadeo's own uploads are the campaign: already in the game
        tracks = [t for t in tracks if t['author'] != 'Nadeo']
    if envs:
        tracks = [t for t in tracks if t['env'] in envs]
    tracks.sort(key=lambda t: -t['plays'])
    if limit:
        tracks = tracks[:limit]
    os.makedirs(os.path.join(out, 'maps'), exist_ok=True)

    def one(t):
        dst = os.path.join(out, 'maps', t['uid'] + '.Map.Gbx')
        if os.path.exists(dst) and os.path.getsize(dst) > 0:
            return 'have'
        try:
            data = get(BUCKET % t['uid'])
        except Exception as e:                      # noqa: BLE001
            return 'fail %s' % e
        if not data.startswith(b'GBX'):
            return 'not a map'
        open(dst + '.tmp', 'wb').write(data)
        os.replace(dst + '.tmp', dst)
        return 'ok'

    res = {}
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for i, r in enumerate(ex.map(one, tracks)):
            k = r.split(' ')[0]
            res[k] = res.get(k, 0) + 1
            if (i + 1) % 100 == 0:
                print('%d / %d %s' % (i + 1, len(tracks), res), flush=True)
    json.dump(tracks, open(os.path.join(out, 'selected.json'), 'w'), indent=0)
    print('done: %s' % res)


def cmd_pack(out, tar_path, limit):
    """Pack the downloaded tracks for the device's ImportTracks folder, most played first,
    named "<rank> <title> <author>__<uid>.Map.Gbx" (the game shows "<title> <author>")."""
    import tarfile
    sel = json.load(open(os.path.join(out, 'selected.json')))
    if limit:
        sel = sel[:limit]
    n = 0
    with tarfile.open(tar_path, 'w') as t:
        for rank, x in enumerate(sel, 1):
            src = os.path.join(out, 'maps', x['uid'] + '.Map.Gbx')
            if not os.path.exists(src):
                continue
            label = re.sub(r'[^A-Za-z0-9 #._-]', '', ('%s %s' % (x['title'], x['author'])).strip())[:31].strip()
            t.add(src, arcname='%04d %s__%s.Map.Gbx' % (rank, label, x['uid']))
            n += 1
    print('%d tracks -> %s' % (n, tar_path))


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ('list', 'fetch', 'pack'):
        print(__doc__)
        sys.exit(2)

    def opt(name, default):
        return a[a.index(name) + 1] if name in a else default
    if a[0] == 'pack':
        cmd_pack(a[1], a[2], int(opt('--limit', '0')))
        return
    if a[0] == 'list':
        cmd_list(a[1], '--all' in a, int(opt('--pages', '100000')))
    else:
        envs = set(opt('--env', '').split(',')) - {''}
        cmd_fetch(a[1], int(opt('--limit', '0')), envs, int(opt('--jobs', '4')), '--with-nadeo' in a)


if __name__ == '__main__':
    main()
