#version 450
layout(location = 0) in vec2 openfl_TextureCoordv;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D bitmap;
layout(set = 0, binding = 1) uniform Params {
    float dim;
    float Directions;
    float Quality;
    float Size;
    vec2 openfl_TextureSize;
};

	

    // GAUSSIAN BLUR SETTINGS
  	
    
     
     

	void main(void)
	{ 
		vec2 uv = openfl_TextureCoordv.xy ;

		float Pi = 6.28318530718; // Pi*2

		vec4 Color = texture( bitmap, uv);
		
		for(float d=0.0; d<Pi; d+=Pi/Directions){
			for(float i=1.0/Quality; i<=1.0; i+=1.0/Quality){

				float ex = (cos(d)*Size*i)/openfl_TextureSize.x;
				float why = (sin(d)*Size*i)/openfl_TextureSize.y;
				Color += texture( bitmap, uv+vec2(ex,why));	
			}
		}
		
		Color /= (dim * Quality) * Directions - 15.0;
		vec4 bloom =  (texture( bitmap, uv)/ dim)+Color;

		fragColor = bloom;

	}
	