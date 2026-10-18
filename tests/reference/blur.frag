precision highp float;
    varying vec2 vUV;
    uniform sampler2D uTex;
    uniform vec2 uStep;
    void main(){
      float w[7];
      w[0]=0.1964; w[1]=0.1760; w[2]=0.1217; w[3]=0.0650; w[4]=0.0267; w[5]=0.0085; w[6]=0.0021;
      vec3 s = texture2D(uTex, vUV).rgb * w[0];
      float weight = w[0];
      for (int i = 1; i < 7; i++){
        vec2 o = uStep * float(i);
        s += texture2D(uTex, vUV + o).rgb * w[i];
        s += texture2D(uTex, vUV - o).rgb * w[i];
        weight += 2.0 * w[i];
      }
      // Correct the prototype's brightness loss without changing its kernel.
      gl_FragColor = vec4(s / weight, 1.0);
    }
