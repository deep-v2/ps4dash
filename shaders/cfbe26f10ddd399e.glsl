uniform mat4 CC_PMatrix;
uniform mat4 CC_MVMatrix;
uniform mat4 CC_MVPMatrix;
uniform vec4 CC_Time;
uniform vec4 CC_SinTime;
uniform vec4 CC_CosTime;
uniform vec4 CC_Random01;
//CC INCLUDES END

															
#ifdef GL_ES												
precision lowp float;										
#endif														
															
varying vec4 v_fragmentColor;								
varying vec2 v_texCoord;									
uniform sampler2D CC_Texture0;								
uniform float CC_alpha_value;								
															
void main()													
{															
	vec4 texColor = texture2D(CC_Texture0, v_texCoord);		
															
	// mimic: glAlphaFunc(GL_GREATER)						
	// pass if ( incoming_pixel >= CC_alpha_value ) => fail if incoming_pixel < CC_alpha_value		
															
	if ( texColor.a <= CC_alpha_value )						
		discard;											
															
	gl_FragColor = texColor * v_fragmentColor;				
}															
