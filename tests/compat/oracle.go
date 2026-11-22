// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/. updateWithNow below is
// copied from Syncthing's lib/protocol/vector.go (Copyright (C) 2015 The
// Syncthing Authors).
//
// Compatibility oracle: produces test cases with Syncthing's own code
// (lib/protocol), for tests/test_vv.c and tests/test_decide.c to check our
// C implementation against. Built and run by scripts/compat-fixtures.sh
// against a Syncthing source tree; its output is committed as
// tests/compat/syncthing-cases.txt.
//
//	go run . SEED COUNT
//
// Line formats (vectors as "id:value,..." or "-"):
//
//	version <syncthing version given by the script>
//	cmp A B ORDER            Vector.Compare (0 equal, 1 greater, 2 lesser, 3 concurrent lesser, 4 concurrent greater)
//	upd V ID NOW RESULT      Vector.updateWithNow (copied: unexported)
//	mrg A B RESULT           Vector.Merge
//	win FA FB RESULT         FileInfo.WinsConflict; F = invalid/modS/modNs/vector
//	icw FN FP RESULT         FileInfo.InConflictWith; F = vector/blocksHashHex/prevBlocksHashHex ("-" for none)
//	sid ID TEXT              ShortID.String
//	bsz SIZE BLOCKSIZE       BlockSize
//	bh HASHES RESULT         BlocksHash over block hashes (hex, joined by ",")
//	fi HEX NAME TYPE SIZE MODS MODNS MODBY DEL INV NOPERM PERM SEQ BSZ VEC NBLOCKS BH PBH
//	                         a FileInfo encoded by ToWire + proto.Marshal, with its fields
//	cfn NAME YYYYMMDDHHMMSS MODBY RESULT   conflictName's formula (filepath.Ext, Format)
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	"google.golang.org/protobuf/proto"
)

var ids = []protocol.ShortID{0, 1, 2, 3, 42, 43, 1 << 32, math.MaxUint64 - 1, math.MaxUint64}
var vals = []uint64{0, 1, 2, 3, 1794240757, 1794240758, math.MaxUint64 - 1, math.MaxUint64}

func randVector(r *rand.Rand) protocol.Vector {
	var v protocol.Vector
	for _, id := range ids {
		if r.Intn(3) == 0 {
			v.Counters = append(v.Counters, protocol.Counter{ID: id, Value: vals[r.Intn(len(vals))]})
		}
	}
	return v
}

func vs(v protocol.Vector) string {
	if len(v.Counters) == 0 {
		return "-"
	}
	parts := make([]string, len(v.Counters))
	for i, c := range v.Counters {
		parts[i] = fmt.Sprintf("%d:%d", uint64(c.ID), c.Value)
	}
	return strings.Join(parts, ",")
}

// A copy of Vector.updateWithNow (unexported in lib/protocol).
func updateWithNow(v protocol.Vector, id protocol.ShortID, now uint64) protocol.Vector {
	for i := range v.Counters {
		if v.Counters[i].ID == id {
			v.Counters[i].Value = max(v.Counters[i].Value+1, now)
			return v
		} else if v.Counters[i].ID > id {
			nv := make([]protocol.Counter, len(v.Counters)+1)
			copy(nv, v.Counters[:i])
			nv[i].ID = id
			nv[i].Value = max(1, now)
			copy(nv[i+1:], v.Counters[i:])
			return protocol.Vector{Counters: nv}
		}
	}
	return protocol.Vector{Counters: append(v.Counters, protocol.Counter{ID: id, Value: max(1, now)})}
}

func randHash(r *rand.Rand) []byte {
	if r.Intn(4) == 0 {
		return nil
	}
	// A small set, so equal hashes occur often.
	h := sha256.Sum256([]byte{byte(r.Intn(3))})
	return h[:]
}

func hx(b []byte) string {
	if len(b) == 0 {
		return "-"
	}
	return hex.EncodeToString(b)
}

func main() {
	seed, _ := strconv.ParseInt(os.Args[1], 10, 64)
	n, _ := strconv.Atoi(os.Args[2])
	r := rand.New(rand.NewSource(seed))
	fmt.Printf("version %s\n", os.Getenv("SYNCTHING_VERSION"))
	for i := 0; i < n; i++ {
		a, b := randVector(r), randVector(r)
		fmt.Printf("cmp %s %s %d\n", vs(a), vs(b), a.Compare(b))
		fmt.Printf("cmp %s %s %d\n", vs(a), vs(a.Copy()), a.Compare(a.Copy()))
		id := ids[r.Intn(len(ids))]
		now := vals[r.Intn(len(vals))]
		fmt.Printf("upd %s %d %d %s\n", vs(a), uint64(id), now, vs(updateWithNow(a.Copy(), id, now)))
		fmt.Printf("mrg %s %s %s\n", vs(a), vs(b), vs(a.Copy().Merge(b)))
		// Concurrent pairs are the interesting ones for the conflict rules.
		if r.Intn(2) == 0 {
			b = updateWithNow(a.Copy(), ids[r.Intn(len(ids))], vals[r.Intn(len(vals))])
			a = updateWithNow(a, ids[r.Intn(len(ids))], vals[r.Intn(len(vals))])
		}
		fa := protocol.FileInfo{Version: a, LocalFlags: 0, ModifiedS: int64(r.Intn(3)), ModifiedNs: int32(r.Intn(2))}
		fb := protocol.FileInfo{Version: b, ModifiedS: int64(r.Intn(3)), ModifiedNs: int32(r.Intn(2))}
		if r.Intn(5) == 0 {
			fa.LocalFlags = protocol.FlagLocalUnsupported
		}
		if r.Intn(5) == 0 {
			fb.LocalFlags = protocol.FlagLocalUnsupported
		}
		inv := func(f protocol.FileInfo) int {
			if f.IsInvalid() {
				return 1
			}
			return 0
		}
		w := 0
		if fa.WinsConflict(fb) {
			w = 1
		}
		fmt.Printf("win %d/%d/%d/%s %d/%d/%d/%s %d\n", inv(fa), fa.ModifiedS, fa.ModifiedNs, vs(fa.Version),
			inv(fb), fb.ModifiedS, fb.ModifiedNs, vs(fb.Version), w)
		fn := protocol.FileInfo{Version: a, BlocksHash: randHash(r), PreviousBlocksHash: randHash(r)}
		fp := protocol.FileInfo{Version: b, BlocksHash: randHash(r), PreviousBlocksHash: randHash(r)}
		c := 0
		if fn.InConflictWith(fp) {
			c = 1
		}
		fmt.Printf("icw %s/%s/%s %s/%s/%s %d\n", vs(fn.Version), hx(fn.BlocksHash), hx(fn.PreviousBlocksHash),
			vs(fp.Version), hx(fp.BlocksHash), hx(fp.PreviousBlocksHash), c)
		sid := protocol.ShortID(r.Uint64())
		if i == 0 {
			sid = 0
		}
		fmt.Printf("sid %d %s\n", uint64(sid), sid.String())
		size := []int64{0, 1, 128 << 10, 2000*(128<<10) - 1, 2000 * (128 << 10), 1 << 40, r.Int63n(1 << 36)}[r.Intn(7)]
		fmt.Printf("bsz %d %d\n", size, protocol.BlockSize(size))
		var blocks []protocol.BlockInfo
		var hs []string
		for k := r.Intn(4); k > 0; k-- {
			h := sha256.Sum256([]byte{byte(r.Intn(256))})
			blocks = append(blocks, protocol.BlockInfo{Hash: h[:]})
			hs = append(hs, hex.EncodeToString(h[:]))
		}
		if len(hs) == 0 {
			hs = []string{"-"}
		}
		fmt.Printf("bh %s %s\n", strings.Join(hs, ","), hex.EncodeToString(protocol.BlocksHash(blocks)))
		emitFileInfo(r)
		names := []string{"a.txt", "dir/b.tar.gz", "noext", ".bashrc", "d.x/y", "d/.hidden.txt", "x."}
		name := names[r.Intn(len(names))]
		t := time.Date(2000+r.Intn(60), time.Month(1+r.Intn(12)), 1+r.Intn(28), r.Intn(24), r.Intn(60), r.Intn(60), 0, time.Local)
		ext := filepath.Ext(name)
		fmt.Printf("cfn %s %s %d %s\n", name, t.Format("20060102150405"), uint64(sid),
			name[:len(name)-len(ext)]+t.Format(".sync-conflict-20060102-150405-")+sid.String()+ext)
	}
}

func emitFileInfo(r *rand.Rand) {
	f := protocol.FileInfo{
		Name:          []string{"a", "dir/file.txt", "x y"}[r.Intn(3)],
		Type:          protocol.FileInfoType(r.Intn(2)),
		Size:          int64(r.Intn(3)) * 131072,
		ModifiedS:     int64(r.Intn(2)) * 1794240757,
		ModifiedNs:    int32(r.Intn(2)) * 999999999,
		ModifiedBy:    protocol.ShortID(r.Uint64() * uint64(r.Intn(2))),
		Deleted:       r.Intn(4) == 0,
		NoPermissions: r.Intn(2) == 0,
		Permissions:   uint32(r.Intn(2)) * 0o644,
		Sequence:      int64(r.Intn(1000)),
		RawBlockSize:  int32(r.Intn(2)) * 131072,
		Version:       randVector(r),
	}
	for off := int64(0); off < f.Size; off += 131072 {
		h := sha256.Sum256([]byte{byte(off >> 17)})
		f.Blocks = append(f.Blocks, protocol.BlockInfo{Offset: off, Size: 131072, Hash: h[:]})
	}
	if f.Size == 0 && f.Type == protocol.FileInfoTypeFile && !f.Deleted {
		e := sha256.Sum256(nil)
		f.Blocks = []protocol.BlockInfo{{Offset: 0, Size: 0, Hash: e[:]}}
	}
	if len(f.Blocks) > 0 {
		f.BlocksHash = protocol.BlocksHash(f.Blocks)
	}
	if r.Intn(2) == 0 {
		h := sha256.Sum256([]byte("prev"))
		f.PreviousBlocksHash = h[:]
	}
	raw, err := proto.Marshal(f.ToWire(false))
	if err != nil {
		panic(err)
	}
	b := func(x bool) int {
		if x {
			return 1
		}
		return 0
	}
	fmt.Printf("fi %s %s %d %d %d %d %d %d %d %d %d %d %d %s %d %s %s\n", hex.EncodeToString(raw),
		strings.ReplaceAll(f.Name, " ", "%20"), f.Type, f.Size, f.ModifiedS, f.ModifiedNs, uint64(f.ModifiedBy),
		b(f.Deleted), b(f.IsInvalid()), b(f.NoPermissions), f.Permissions, f.Sequence, f.RawBlockSize,
		vs(f.Version), len(f.Blocks), hx(f.BlocksHash), hx(f.PreviousBlocksHash))
}
