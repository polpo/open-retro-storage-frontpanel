// Incremental SHA256 implementation that supports chunked processing
class SHA256Context {
	constructor() {
		this.buffer = new Uint8Array(64); // 512-bit buffer
		this.bufferLength = 0;
		this.bytesProcessed = 0;

		// Initial hash values (first 32 bits of the fractional parts of the square roots of the first 8 primes)
		this.hash = [
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
		];

		// Round constants (first 32 bits of the fractional parts of the cube roots of the first 64 primes)
		this.k = SHA256Context.k || this.initializeConstants();
	}

	initializeConstants() {
		var mathPow = Math.pow;
		var maxWord = mathPow(2, 32);
		var k = [];
		var primeCounter = 0;
		var isComposite = {};

		for (var candidate = 2; primeCounter < 64; candidate++) {
			if (!isComposite[candidate]) {
				for (var i = 0; i < 313; i += candidate) {
					isComposite[i] = candidate;
				}
				k[primeCounter++] = (mathPow(candidate, 1/3)*maxWord)|0;
			}
		}

		SHA256Context.k = k; // Cache for future use
		return k;
	}

	rightRotate(value, amount) {
		return (value>>>amount) | (value<<(32 - amount));
	}

	processBlock(block) {
		var w = new Array(64);
		var i;

		// Convert block to 32-bit words
		for (i = 0; i < 16; i++) {
			w[i] = (block[i * 4] << 24) | (block[i * 4 + 1] << 16) |
			       (block[i * 4 + 2] << 8) | block[i * 4 + 3];
		}

		// Extend the message schedule
		for (i = 16; i < 64; i++) {
			var w15 = w[i - 15];
			var w2 = w[i - 2];
			var s0 = this.rightRotate(w15, 7) ^ this.rightRotate(w15, 18) ^ (w15 >>> 3);
			var s1 = this.rightRotate(w2, 17) ^ this.rightRotate(w2, 19) ^ (w2 >>> 10);
			w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
		}

		// Working variables
		var a = this.hash[0], b = this.hash[1], c = this.hash[2], d = this.hash[3];
		var e = this.hash[4], f = this.hash[5], g = this.hash[6], h = this.hash[7];

		// Main loop
		for (i = 0; i < 64; i++) {
			var S1 = this.rightRotate(e, 6) ^ this.rightRotate(e, 11) ^ this.rightRotate(e, 25);
			var ch = (e & f) ^ ((~e) & g);
			var temp1 = (h + S1 + ch + this.k[i] + w[i]) | 0;
			var S0 = this.rightRotate(a, 2) ^ this.rightRotate(a, 13) ^ this.rightRotate(a, 22);
			var maj = (a & b) ^ (a & c) ^ (b & c);
			var temp2 = (S0 + maj) | 0;

			h = g;
			g = f;
			f = e;
			e = (d + temp1) | 0;
			d = c;
			c = b;
			b = a;
			a = (temp1 + temp2) | 0;
		}

		// Add to hash
		this.hash[0] = (this.hash[0] + a) | 0;
		this.hash[1] = (this.hash[1] + b) | 0;
		this.hash[2] = (this.hash[2] + c) | 0;
		this.hash[3] = (this.hash[3] + d) | 0;
		this.hash[4] = (this.hash[4] + e) | 0;
		this.hash[5] = (this.hash[5] + f) | 0;
		this.hash[6] = (this.hash[6] + g) | 0;
		this.hash[7] = (this.hash[7] + h) | 0;
	}

	update(data) {
		if (!(data instanceof Uint8Array)) {
			data = new Uint8Array(data);
		}

		var offset = 0;
		var length = data.length;

		while (length > 0) {
			// Fill buffer
			var toCopy = Math.min(length, 64 - this.bufferLength);
			this.buffer.set(data.subarray(offset, offset + toCopy), this.bufferLength);
			this.bufferLength += toCopy;
			this.bytesProcessed += toCopy;
			offset += toCopy;
			length -= toCopy;

			// Process complete blocks
			if (this.bufferLength === 64) {
				this.processBlock(this.buffer);
				this.bufferLength = 0;
			}
		}
	}

	finalize() {
		var bitLength = this.bytesProcessed * 8;
		var mathPow = Math.pow;
		var maxWord = mathPow(2, 32);

		// Add padding
		var paddingLength = (this.bufferLength < 56) ? (56 - this.bufferLength) : (120 - this.bufferLength);
		var padding = new Uint8Array(paddingLength + 8);
		padding[0] = 0x80;

		// Add length as 64-bit big-endian integer at the end
		var lengthView = new DataView(padding.buffer, paddingLength);
		lengthView.setUint32(0, Math.floor(bitLength / maxWord), false);
		lengthView.setUint32(4, bitLength, false);

		// Process final blocks
		this.update(padding);

		// Convert hash to ArrayBuffer
		var resultBuffer = new ArrayBuffer(32);
		var resultView = new DataView(resultBuffer);
		for (var i = 0; i < 8; i++) {
			resultView.setUint32(i * 4, this.hash[i], false);
		}

		return resultBuffer;
	}
}

// Simple wrapper for backward compatibility with existing code
function sha256(buffer) {
	var ctx = new SHA256Context();
	ctx.update(new Uint8Array(buffer));
	return ctx.finalize();
}