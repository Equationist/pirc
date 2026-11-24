with Text_IO;
with Calendar;
procedure Bench03_Nbody is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   package Flt_IO is new Text_IO.Float_IO(Long_Float);

   Pi : constant Long_Float := 3.14159265358979323846;
   Solar_Mass : constant Long_Float := 4.0 * Pi * Pi;
   Days_Per_Year : constant Long_Float := 365.24;

   type Body_Rec is record
      X, Y, Z    : Long_Float;
      Vx, Vy, Vz : Long_Float;
      Mass       : Long_Float;
   end record;

   type Body_Arr is array (Integer range 1 .. 5) of Body_Rec;
   Bodies : Body_Arr;

   Steps : constant Integer := 200_000;
   Iters : constant Integer := 7;
   T0, T1 : Calendar.Time;
   Ms : Long_Float;
   Best_Ms : Long_Float := 1.0E9;
   E_Final : Long_Float := 0.0;

   function Sqrt_Newton(X : Long_Float) return Long_Float is
      G : Long_Float;
   begin
      if X <= 0.0 then
         return 0.0;
      end if;
      G := X * 0.5 + 0.5;
      for K in 1 .. 14 loop
         G := 0.5 * (G + X / G);
      end loop;
      return G;
   end Sqrt_Newton;

   procedure Init_Bodies is
      Px, Py, Pz : Long_Float := 0.0;
   begin
      Bodies(1) := (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, Solar_Mass);
      Bodies(2) := (4.84143144246472090, -1.16032004402742839, -0.103622044471123109,
                    1.66007664274403694E-3 * Days_Per_Year,
                    7.69901118419740425E-3 * Days_Per_Year,
                    -6.90460016972063023E-5 * Days_Per_Year,
                    9.54791938424326609E-4 * Solar_Mass);
      Bodies(3) := (8.34336671824457987, 4.12479856412430479, -0.403523417114321381,
                    -2.76742510726862411E-3 * Days_Per_Year,
                    4.99852801234917238E-3 * Days_Per_Year,
                    2.30417297573763929E-5 * Days_Per_Year,
                    2.85885980666130812E-4 * Solar_Mass);
      Bodies(4) := (12.8943695621391310, -15.1111514016986312, -0.223307578892655734,
                    2.96460137564761618E-3 * Days_Per_Year,
                    2.37847173959480950E-3 * Days_Per_Year,
                    -2.96589568540237556E-5 * Days_Per_Year,
                    4.36624404335156298E-5 * Solar_Mass);
      Bodies(5) := (15.3796971148509165, -25.9193146099879641, 0.179258772950371181,
                    2.68067772490389322E-3 * Days_Per_Year,
                    1.62824170038242295E-3 * Days_Per_Year,
                    -9.51592254519715870E-5 * Days_Per_Year,
                    5.15138902046611451E-5 * Solar_Mass);
      for I in 1 .. 5 loop
         Px := Px + Bodies(I).Vx * Bodies(I).Mass;
         Py := Py + Bodies(I).Vy * Bodies(I).Mass;
         Pz := Pz + Bodies(I).Vz * Bodies(I).Mass;
      end loop;
      Bodies(1).Vx := -Px / Solar_Mass;
      Bodies(1).Vy := -Py / Solar_Mass;
      Bodies(1).Vz := -Pz / Solar_Mass;
   end Init_Bodies;

   procedure Advance(Dt : Long_Float) is
      Dx, Dy, Dz, D2, Dist, Mag : Long_Float;
   begin
      for I in 1 .. 4 loop
         for J in I + 1 .. 5 loop
            Dx := Bodies(I).X - Bodies(J).X;
            Dy := Bodies(I).Y - Bodies(J).Y;
            Dz := Bodies(I).Z - Bodies(J).Z;
            D2 := Dx * Dx + Dy * Dy + Dz * Dz;
            Dist := Sqrt_Newton(D2);
            Mag := Dt / (D2 * Dist);
            Bodies(I).Vx := Bodies(I).Vx - Dx * Bodies(J).Mass * Mag;
            Bodies(I).Vy := Bodies(I).Vy - Dy * Bodies(J).Mass * Mag;
            Bodies(I).Vz := Bodies(I).Vz - Dz * Bodies(J).Mass * Mag;
            Bodies(J).Vx := Bodies(J).Vx + Dx * Bodies(I).Mass * Mag;
            Bodies(J).Vy := Bodies(J).Vy + Dy * Bodies(I).Mass * Mag;
            Bodies(J).Vz := Bodies(J).Vz + Dz * Bodies(I).Mass * Mag;
         end loop;
      end loop;
      for I in 1 .. 5 loop
         Bodies(I).X := Bodies(I).X + Dt * Bodies(I).Vx;
         Bodies(I).Y := Bodies(I).Y + Dt * Bodies(I).Vy;
         Bodies(I).Z := Bodies(I).Z + Dt * Bodies(I).Vz;
      end loop;
   end Advance;

   function Energy return Long_Float is
      E, Dx, Dy, Dz, Dist : Long_Float := 0.0;
   begin
      for I in 1 .. 5 loop
         E := E + 0.5 * Bodies(I).Mass *
              (Bodies(I).Vx * Bodies(I).Vx + Bodies(I).Vy * Bodies(I).Vy + Bodies(I).Vz * Bodies(I).Vz);
         for J in I + 1 .. 5 loop
            Dx := Bodies(I).X - Bodies(J).X;
            Dy := Bodies(I).Y - Bodies(J).Y;
            Dz := Bodies(I).Z - Bodies(J).Z;
            Dist := Sqrt_Newton(Dx * Dx + Dy * Dy + Dz * Dz);
            E := E - (Bodies(I).Mass * Bodies(J).Mass) / Dist;
         end loop;
      end loop;
      return E;
   end Energy;

begin
   for It in 1 .. Iters loop
      Init_Bodies;
      T0 := Calendar.Clock;
      for S in 1 .. Steps loop
         Advance(0.01);
      end loop;
      E_Final := Energy;
      T1 := Calendar.Clock;
      Ms := Long_Float(Calendar."-"(T1, T0)) * 1000.0;
      if Ms < Best_Ms then
         Best_Ms := Ms;
      end if;
   end loop;
   Int_IO.Put(Integer(E_Final * 1_000_000.0), Width => 0);
   Text_IO.Put(" ms=");
   Flt_IO.Put(Best_Ms, Fore => 1, Aft => 3, Exp => 0);
   Text_IO.New_Line;
end Bench03_Nbody;
