#version 450
layout(location = 0) in vec2 openfl_TextureCoordv;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D bitmap;
layout(set = 0, binding = 1) uniform Params {
    float stronk;
    float iTime;
    vec2 pixel;
    vec2 openfl_TextureSize;
};

    

	
	
	

	const bool allowWiggle = true;


	vec2 uvp(vec2 uv) {
		return clamp(uv, 0.0, 1.0);
	}

	float outCirc(float t) {
    	return sqrt(-t * t + 2.0 * t);
	}

	float rand(vec2 co) {
		return fract(sin(dot(co.xy,vec2(12.9898,78.233))) * 43758.5453);
	}

	void main() {
		vec3 col;
		float amp;
		
		amp = stronk;

		for (int i = 0; i < 3; i++) {

			vec2 size = openfl_TextureSize.xy / pixel;
			vec2 uv = floor(openfl_TextureCoordv.xy * size) / size;
			
			if (allowWiggle) {
				//uv += vec2(sin(iTime + float(i) + amp), cos(iTime + float(i) + (amp*0.5))) * (amp*0.5) * 0.2;
				uv += vec2(sin(float(i) * amp), cos(float(i) * amp)) * amp * 0.05;
			}

			vec3 texOrig = texture(bitmap, uvp(uv)).rgb;
			
			uv.x += (rand(vec2(uv.y + float(i), iTime)) * 2.0 - 1.0) * amp * 0.8 * (texOrig[i] + 0.2);
			uv.y += (rand(vec2(uv.x, iTime + float(i))) * 2.0 - 1.0) * amp * 0.1 * (texOrig[i] + 0.2);
			
			vec3 tex = texture(bitmap, uvp(uv)).rgb;

			tex += abs(tex[i] - texOrig[i]);
			
			tex *= rand(uv) * amp + 1.0;

			if (i != 0) {
				//tex = fract(tex);
			}

			
			
			
			col[i] = tex[i];
		}


    
		fragColor = vec4(col,texture(bitmap,openfl_TextureCoordv).a);
	}

	
	
	